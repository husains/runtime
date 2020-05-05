// Licensed to the .NET Foundation under one or more agreements.
// The .NET Foundation licenses this file to you under the MIT license.
// See the LICENSE file in the project root for more information.

// Runtime headers
#include "common.h"
#include "rcwrefcache.h"
#include "rcwwalker.h"
#include "olecontexthelpers.h"
#include "finalizerthread.h"

// Interop library header
#include <interoplibimports.h>

#include "interoplibinterface.h"

using CreateObjectFlags = InteropLib::Com::CreateObjectFlags;
using CreateComInterfaceFlags = InteropLib::Com::CreateComInterfaceFlags;

namespace
{
    // This class is used to track the external object within the runtime.
    struct ExternalObjectContext
    {
        static const DWORD InvalidSyncBlockIndex;

        void* Identity;
        void* ThreadContext;
        DWORD SyncBlockIndex;

        enum
        {
            Flags_None = 0,
            Flags_Collected = 1,
            Flags_ReferenceTracker = 2,
            Flags_InCache = 4,
        };
        DWORD Flags;

        static void Construct(
            _Out_ ExternalObjectContext* cxt,
            _In_ IUnknown* identity,
            _In_opt_ void* threadContext,
            _In_ DWORD syncBlockIndex,
            _In_ DWORD flags)
        {
            CONTRACTL
            {
                NOTHROW;
                GC_NOTRIGGER;
                MODE_ANY;
                PRECONDITION(cxt != NULL);
                PRECONDITION(threadContext != NULL);
                PRECONDITION(syncBlockIndex != InvalidSyncBlockIndex);
            }
            CONTRACTL_END;

            cxt->Identity = (void*)identity;
            cxt->ThreadContext = threadContext;
            cxt->SyncBlockIndex = syncBlockIndex;
            cxt->Flags = flags;
        }

        bool IsSet(_In_ DWORD f) const
        {
            return ((Flags & f) == f);
        }

        bool IsActive() const
        {
            return !IsSet(Flags_Collected)
                && (SyncBlockIndex != InvalidSyncBlockIndex);
        }

        void MarkCollected()
        {
            _ASSERTE(GCHeapUtilities::IsGCInProgress());
            SyncBlockIndex = InvalidSyncBlockIndex;
            Flags |= Flags_Collected;
        }

        OBJECTREF GetObjectRef()
        {
            CONTRACTL
            {
                NOTHROW;
                GC_NOTRIGGER;
                MODE_COOPERATIVE;
            }
            CONTRACTL_END;

            _ASSERTE(IsActive());
            return ObjectToOBJECTREF(g_pSyncTable[SyncBlockIndex].m_Object);
        }
    };

    const DWORD ExternalObjectContext::InvalidSyncBlockIndex = 0; // See syncblk.h

    static_assert((sizeof(ExternalObjectContext) % sizeof(void*)) == 0, "Keep context pointer size aligned");

    // Holder for a External Wrapper Result
    struct ExternalWrapperResultHolder
    {
        InteropLib::Com::ExternalWrapperResult Result;
        ExternalWrapperResultHolder()
            : Result{}
        { }
        ~ExternalWrapperResultHolder()
        {
            if (Result.Context != NULL)
                InteropLib::Com::DestroyWrapperForExternal(Result.Context);
        }
        InteropLib::Com::ExternalWrapperResult* operator&()
        {
            return &Result;
        }
        ExternalObjectContext* GetContext()
        {
            return static_cast<ExternalObjectContext*>(Result.Context);
        }
        ExternalObjectContext* DetachContext()
        {
            ExternalObjectContext* t = GetContext();
            Result.Context = NULL;
            return t;
        }
    };

    using ExtObjCxtRefCache = RCWRefCache;

    class ExtObjCxtCache
    {
        static Volatile<ExtObjCxtCache*> g_Instance;

    public: // static
        static ExtObjCxtCache* GetInstanceNoThrow() noexcept
        {
            CONTRACT(ExtObjCxtCache*)
            {
                NOTHROW;
                GC_NOTRIGGER;
                POSTCONDITION(CheckPointer(RETVAL, NULL_OK));
            }
            CONTRACT_END;

            RETURN g_Instance;
        }

        static ExtObjCxtCache* GetInstance()
        {
            CONTRACT(ExtObjCxtCache*)
            {
                THROWS;
                GC_NOTRIGGER;
                POSTCONDITION(RETVAL != NULL);
            }
            CONTRACT_END;

            if (g_Instance.Load() == NULL)
            {
                ExtObjCxtCache* instMaybe = new ExtObjCxtCache();

                // Attempt to set the global instance.
                if (NULL != FastInterlockCompareExchangePointer(&g_Instance, instMaybe, NULL))
                    delete instMaybe;
            }

            RETURN g_Instance;
        }

    public: // Inner class definitions
        class Traits : public DefaultSHashTraits<ExternalObjectContext *>
        {
        public:
            using key_t = void*;
            static const key_t GetKey(_In_ element_t e) { LIMITED_METHOD_CONTRACT; return (key_t)e->Identity; }
            static count_t Hash(_In_ key_t key) { LIMITED_METHOD_CONTRACT; return (count_t)(size_t)key; }
            static bool Equals(_In_ key_t lhs, _In_ key_t rhs) { LIMITED_METHOD_CONTRACT; return (lhs == rhs); }
        };

        // Alias some useful types
        using Element = SHash<Traits>::element_t;
        using Iterator = SHash<Traits>::Iterator;

        class LockHolder : public CrstHolder
        {
        public:
            LockHolder(_In_ ExtObjCxtCache *cache)
                : CrstHolder(&cache->_lock)
            {
                // This cache must be locked in Cooperative mode
                // since releases of wrappers can occur during a GC.
                CONTRACTL
                {
                    NOTHROW;
                    GC_NOTRIGGER;
                    MODE_COOPERATIVE;
                }
                CONTRACTL_END;
            }
        };

    private:
        friend struct InteropLibImports::RuntimeCallContext;
        SHash<Traits> _hashMap;
        Crst _lock;
        ExtObjCxtRefCache* _refCache;

        ExtObjCxtCache()
            : _lock(CrstExternalObjectContextCache, CRST_UNSAFE_COOPGC)
            , _refCache(GetAppDomain()->GetRCWRefCache())
        { }
        ~ExtObjCxtCache() = default;

    public:
#if _DEBUG
        bool IsLockHeld()
        {
            WRAPPER_NO_CONTRACT;
            return (_lock.OwnedByCurrentThread() != FALSE);
        }
#endif // _DEBUG

        // Get the associated reference cache with this external object cache.
        ExtObjCxtRefCache* GetRefCache()
        {
            WRAPPER_NO_CONTRACT;
            return _refCache;
        }

        // Create a managed IEnumerable instance for this collection.
        // The collection should respect the supplied arguments.
        //        withFlags - If Flag_None, then ignore. Otherwise objects must have these flags.
        //    threadContext - The object must be associated with the supplied thread context.
        //
        // [TODO] Performance improvement should be made here to provide a custom IEnumerable
        // instead of a managed array.
        OBJECTREF CreateManagedEnumerable(_In_ DWORD withFlags, _In_opt_ void* threadContext)
        {
            CONTRACT(OBJECTREF)
            {
                THROWS;
                GC_TRIGGERS;
                MODE_COOPERATIVE;
                PRECONDITION(!IsLockHeld());
                POSTCONDITION(RETVAL != NULL);
            }
            CONTRACT_END;

            DWORD objCount;
            DWORD objCountMax;

            struct
            {
                PTRARRAYREF arrRef;
                PTRARRAYREF arrRefTmp;
            } gc;
            ::ZeroMemory(&gc, sizeof(gc));
            GCPROTECT_BEGIN(gc);

            {
                LockHolder lock(this);
                objCountMax = _hashMap.GetCount();
            }

            // Allocate the max number of objects needed.
            gc.arrRef = (PTRARRAYREF)AllocateObjectArray(objCountMax, g_pObjectClass);

            // Populate the array
            {
                LockHolder lock(this);
                Iterator curr = _hashMap.Begin();
                Iterator end = _hashMap.End();

                ExternalObjectContext* inst;
                for (objCount = 0; curr != end && objCount < objCountMax; objCount++, curr++)
                {
                    inst = *curr;

                    // Only add objects that are in the correct thread
                    // context and have the appropriate flags set.
                    if (inst->ThreadContext == threadContext
                        && (withFlags == ExternalObjectContext::Flags_None || inst->IsSet(withFlags)))
                    {
                        // Separate the wrapper from the tracker runtime prior to
                        // passing this onto the caller. This call is okay to make
                        // even if the instance isn't from the tracker runtime.
                        InteropLib::Com::SeparateWrapperFromTrackerRuntime(inst);
                        gc.arrRef->SetAt(objCount, inst->GetObjectRef());
                        STRESS_LOG1(LF_INTEROP, LL_INFO100, "Add EOC to Enumerable: 0x%p\n", inst);
                    }
                }
            }

            // Make the array the correct size
            if (objCount < objCountMax)
            {
                gc.arrRefTmp = (PTRARRAYREF)AllocateObjectArray(objCount, g_pObjectClass);

                SIZE_T elementSize = gc.arrRef->GetComponentSize();

                void *src = gc.arrRef->GetDataPtr();
                void *dest = gc.arrRefTmp->GetDataPtr();

                _ASSERTE(sizeof(Object*) == elementSize && "Assumption invalidated in memmoveGCRefs() usage");
                memmoveGCRefs(dest, src, objCount * elementSize);
                gc.arrRef = gc.arrRefTmp;
            }

            GCPROTECT_END();

            RETURN gc.arrRef;
        }

        ExternalObjectContext* Find(_In_ IUnknown* instance)
        {
            CONTRACT(ExternalObjectContext*)
            {
                NOTHROW;
                GC_NOTRIGGER;
                MODE_COOPERATIVE;
                PRECONDITION(IsLockHeld());
                PRECONDITION(instance != NULL);
                POSTCONDITION(CheckPointer(RETVAL, NULL_OK));
            }
            CONTRACT_END;

            // Forbid the GC from messing with the hash table.
            GCX_FORBID();

            RETURN _hashMap.Lookup(instance);
        }

        ExternalObjectContext* Add(_In_ ExternalObjectContext* cxt)
        {
            CONTRACT(ExternalObjectContext*)
            {
                THROWS;
                GC_NOTRIGGER;
                MODE_COOPERATIVE;
                PRECONDITION(IsLockHeld());
                PRECONDITION(!Traits::IsNull(cxt) && !Traits::IsDeleted(cxt));
                PRECONDITION(cxt->Identity != NULL);
                PRECONDITION(Find(static_cast<IUnknown*>(cxt->Identity)) == NULL);
                POSTCONDITION(RETVAL == cxt);
            }
            CONTRACT_END;

            _hashMap.Add(cxt);
            RETURN cxt;
        }

        ExternalObjectContext* FindOrAdd(_In_ IUnknown* key, _In_ ExternalObjectContext* newCxt)
        {
            CONTRACT(ExternalObjectContext*)
            {
                THROWS;
                GC_NOTRIGGER;
                MODE_COOPERATIVE;
                PRECONDITION(IsLockHeld());
                PRECONDITION(key != NULL);
                PRECONDITION(!Traits::IsNull(newCxt) && !Traits::IsDeleted(newCxt));
                PRECONDITION(key == newCxt->Identity);
                POSTCONDITION(CheckPointer(RETVAL));
            }
            CONTRACT_END;

            // Forbid the GC from messing with the hash table.
            GCX_FORBID();

            ExternalObjectContext* cxt = Find(key);
            if (cxt == NULL)
                cxt = Add(newCxt);

            RETURN cxt;
        }

        void Remove(_In_ ExternalObjectContext* cxt)
        {
            CONTRACTL
            {
                NOTHROW;
                GC_NOTRIGGER;
                MODE_ANY;
                PRECONDITION(!Traits::IsNull(cxt) && !Traits::IsDeleted(cxt));
                PRECONDITION(cxt->Identity != NULL);

                // The GC thread doesn't have to take the lock
                // since all other threads access in cooperative mode
                PRECONDITION(
                    (IsLockHeld() && GetThread()->PreemptiveGCDisabled())
                    || Debug_IsLockedViaThreadSuspension());
            }
            CONTRACTL_END;

            _hashMap.Remove(cxt->Identity);
        }
    };

    // Global instance of the external object cache
    Volatile<ExtObjCxtCache*> ExtObjCxtCache::g_Instance;

    // Indicator for if a ComWrappers implementation is globally registered
    bool g_IsGlobalComWrappersRegistered;

    // Defined handle types for the specific object uses.
    const HandleType InstanceHandleType{ HNDTYPE_STRONG };

    // Scenarios for ComWrappers usage.
    // These values should match the managed definition in ComWrappers.
    enum class ComWrappersScenario
    {
        Instance = 0,
        TrackerSupportGlobalInstance = 1,
        MarshallingGlobalInstance = 2,
    };

    void* CallComputeVTables(
        _In_ ComWrappersScenario scenario,
        _In_ OBJECTREF* implPROTECTED,
        _In_ OBJECTREF* instancePROTECTED,
        _In_ INT32 flags,
        _Out_ DWORD* vtableCount)
    {
        CONTRACTL
        {
            THROWS;
            MODE_COOPERATIVE;
            PRECONDITION(implPROTECTED != NULL);
            PRECONDITION(instancePROTECTED != NULL);
            PRECONDITION(vtableCount != NULL);
        }
        CONTRACTL_END;

        void* vtables = NULL;

        PREPARE_NONVIRTUAL_CALLSITE(METHOD__COMWRAPPERS__COMPUTE_VTABLES);
        DECLARE_ARGHOLDER_ARRAY(args, 5);
        args[ARGNUM_0]  = DWORD_TO_ARGHOLDER(scenario);
        args[ARGNUM_1]  = OBJECTREF_TO_ARGHOLDER(*implPROTECTED);
        args[ARGNUM_2]  = OBJECTREF_TO_ARGHOLDER(*instancePROTECTED);
        args[ARGNUM_3]  = DWORD_TO_ARGHOLDER(flags);
        args[ARGNUM_4]  = PTR_TO_ARGHOLDER(vtableCount);
        CALL_MANAGED_METHOD(vtables, void*, args);

        return vtables;
    }

    OBJECTREF CallCreateObject(
        _In_ ComWrappersScenario scenario,
        _In_ OBJECTREF* implPROTECTED,
        _In_ IUnknown* externalComObject,
        _In_ INT32 flags)
    {
        CONTRACTL
        {
            THROWS;
            MODE_COOPERATIVE;
            PRECONDITION(implPROTECTED != NULL);
            PRECONDITION(externalComObject != NULL);
        }
        CONTRACTL_END;

        OBJECTREF retObjRef;

        PREPARE_NONVIRTUAL_CALLSITE(METHOD__COMWRAPPERS__CREATE_OBJECT);
        DECLARE_ARGHOLDER_ARRAY(args, 4);
        args[ARGNUM_0]  = DWORD_TO_ARGHOLDER(scenario);
        args[ARGNUM_1]  = OBJECTREF_TO_ARGHOLDER(*implPROTECTED);
        args[ARGNUM_2]  = PTR_TO_ARGHOLDER(externalComObject);
        args[ARGNUM_3]  = DWORD_TO_ARGHOLDER(flags);
        CALL_MANAGED_METHOD(retObjRef, OBJECTREF, args);

        return retObjRef;
    }

    void CallReleaseObjects(
        _In_ OBJECTREF* implPROTECTED,
        _In_ OBJECTREF* objsEnumPROTECTED)
    {
        CONTRACTL
        {
            THROWS;
            MODE_COOPERATIVE;
            PRECONDITION(implPROTECTED != NULL);
            PRECONDITION(objsEnumPROTECTED != NULL);
        }
        CONTRACTL_END;

        PREPARE_NONVIRTUAL_CALLSITE(METHOD__COMWRAPPERS__RELEASE_OBJECTS);
        DECLARE_ARGHOLDER_ARRAY(args, 2);
        args[ARGNUM_0]  = OBJECTREF_TO_ARGHOLDER(*implPROTECTED);
        args[ARGNUM_1]  = OBJECTREF_TO_ARGHOLDER(*objsEnumPROTECTED);
        CALL_MANAGED_METHOD_NORET(args);
    }

    int CallICustomQueryInterface(
        _In_ OBJECTREF* implPROTECTED,
        _In_ REFGUID iid,
        _Outptr_result_maybenull_ void** ppObject)
    {
        CONTRACTL
        {
            THROWS;
            MODE_COOPERATIVE;
            PRECONDITION(implPROTECTED != NULL);
            PRECONDITION(ppObject != NULL);
        }
        CONTRACTL_END;

        int result;

        PREPARE_NONVIRTUAL_CALLSITE(METHOD__COMWRAPPERS__CALL_ICUSTOMQUERYINTERFACE);
        DECLARE_ARGHOLDER_ARRAY(args, 3);
        args[ARGNUM_0]  = OBJECTREF_TO_ARGHOLDER(*implPROTECTED);
        args[ARGNUM_1]  = PTR_TO_ARGHOLDER(&iid);
        args[ARGNUM_2]  = PTR_TO_ARGHOLDER(ppObject);
        CALL_MANAGED_METHOD(result, int, args);

        return result;
    }

    bool TryGetOrCreateComInterfaceForObjectInternal(
        _In_opt_ OBJECTREF impl,
        _In_ OBJECTREF instance,
        _In_ CreateComInterfaceFlags flags,
        _In_ ComWrappersScenario scenario,
        _Outptr_ void** wrapperRaw)
    {
        CONTRACT(bool)
        {
            THROWS;
            MODE_COOPERATIVE;
            PRECONDITION(instance != NULL);
            PRECONDITION(wrapperRaw != NULL);
            PRECONDITION((impl != NULL && scenario == ComWrappersScenario::Instance)|| (impl == NULL && scenario != ComWrappersScenario::Instance));
        }
        CONTRACT_END;

        HRESULT hr;

        SafeComHolder<IUnknown> newWrapper;
        void* wrapperRawMaybe = NULL;

        struct
        {
            OBJECTREF implRef;
            OBJECTREF instRef;
        } gc;
        ::ZeroMemory(&gc, sizeof(gc));
        GCPROTECT_BEGIN(gc);

        gc.implRef = impl;
        gc.instRef = instance;

        // Check the object's SyncBlock for a managed object wrapper.
        SyncBlock* syncBlock = gc.instRef->GetSyncBlock();
        InteropSyncBlockInfo* interopInfo = syncBlock->GetInteropInfo();
        _ASSERTE(syncBlock->IsPrecious());

        // Query the associated InteropSyncBlockInfo for an existing managed object wrapper.
        if (!interopInfo->TryGetManagedObjectComWrapper(&wrapperRawMaybe))
        {
            // Compute VTables for the new existing COM object using the supplied COM Wrappers implementation.
            //
            // N.B. Calling to compute the associated VTables is perhaps early since no lock
            // is taken. However, a key assumption here is that the returned memory will be
            // idempotent for the same object.
            DWORD vtableCount;
            void* vtables = CallComputeVTables(scenario, &gc.implRef, &gc.instRef, flags, &vtableCount);

            // Re-query the associated InteropSyncBlockInfo for an existing managed object wrapper.
            if (!interopInfo->TryGetManagedObjectComWrapper(&wrapperRawMaybe)
                && ((vtables != nullptr && vtableCount > 0) || (vtableCount == 0)))
            {
                OBJECTHANDLE instHandle = GetAppDomain()->CreateTypedHandle(gc.instRef, InstanceHandleType);

                // Call the InteropLib and create the associated managed object wrapper.
                hr = InteropLib::Com::CreateWrapperForObject(
                    instHandle,
                    vtableCount,
                    vtables,
                    flags,
                    &newWrapper);
                if (FAILED(hr))
                {
                    DestroyHandleCommon(instHandle, InstanceHandleType);
                    COMPlusThrowHR(hr);
                }
                _ASSERTE(!newWrapper.IsNull());

                // Try setting the newly created managed object wrapper on the InteropSyncBlockInfo.
                if (!interopInfo->TrySetManagedObjectComWrapper(newWrapper))
                {
                    // The new wrapper couldn't be set which means a wrapper already exists.
                    newWrapper.Release();

                    // If the managed object wrapper couldn't be set, then
                    // it should be possible to get the current one.
                    if (!interopInfo->TryGetManagedObjectComWrapper(&wrapperRawMaybe))
                    {
                        UNREACHABLE();
                    }
                }
            }
        }

        // Determine what to return.
        if (!newWrapper.IsNull())
        {
            // A new managed object wrapper was created, remove the object from the holder.
            // No AddRef() here since the wrapper should be created with a reference.
            wrapperRawMaybe = newWrapper.Extract();
            STRESS_LOG1(LF_INTEROP, LL_INFO100, "Created MOW: 0x%p\n", wrapperRawMaybe);
        }
        else if (wrapperRawMaybe != NULL)
        {
            // It is possible the supplied wrapper is no longer valid. If so, reactivate the
            // wrapper using the protected OBJECTREF.
            IUnknown* wrapper = static_cast<IUnknown*>(wrapperRawMaybe);
            hr = InteropLib::Com::IsActiveWrapper(wrapper);
            if (hr == S_FALSE)
            {
                STRESS_LOG1(LF_INTEROP, LL_INFO100, "Reactivating MOW: 0x%p\n", wrapperRawMaybe);
                OBJECTHANDLE h = GetAppDomain()->CreateTypedHandle(gc.instRef, InstanceHandleType);
                hr = InteropLib::Com::ReactivateWrapper(wrapper, static_cast<InteropLib::OBJECTHANDLE>(h));
            }

            if (FAILED(hr))
                COMPlusThrowHR(hr);
        }

        GCPROTECT_END();

        *wrapperRaw = wrapperRawMaybe;
        RETURN (wrapperRawMaybe != NULL);
    }

    bool TryGetOrCreateObjectForComInstanceInternal(
        _In_opt_ OBJECTREF impl,
        _In_ IUnknown* identity,
        _In_ CreateObjectFlags flags,
        _In_ ComWrappersScenario scenario,
        _In_opt_ OBJECTREF wrapperMaybe,
        _Out_ OBJECTREF* objRef)
    {
        CONTRACT(bool)
        {
            THROWS;
            MODE_COOPERATIVE;
            PRECONDITION(identity != NULL);
            PRECONDITION(objRef != NULL);
            PRECONDITION((impl != NULL && scenario == ComWrappersScenario::Instance) || (impl == NULL && scenario != ComWrappersScenario::Instance));
        }
        CONTRACT_END;

        HRESULT hr;
        ExternalObjectContext* extObjCxt = NULL;

        struct
        {
            OBJECTREF implRef;
            OBJECTREF wrapperMaybeRef;
            OBJECTREF objRefMaybe;
        } gc;
        ::ZeroMemory(&gc, sizeof(gc));
        GCPROTECT_BEGIN(gc);

        gc.implRef = impl;
        gc.wrapperMaybeRef = wrapperMaybe;

        ExtObjCxtCache* cache = ExtObjCxtCache::GetInstance();
        InteropLib::OBJECTHANDLE handle = NULL;

        // Check if the user requested a unique instance.
        bool uniqueInstance = !!(flags & CreateObjectFlags::CreateObjectFlags_UniqueInstance);
        if (!uniqueInstance)
        {
            // Query the external object cache
            ExtObjCxtCache::LockHolder lock(cache);
            extObjCxt = cache->Find(identity);

            // If is no object found in the cache, check if the object COM instance is actually the CCW
            // representing a managed object. For the scenario of marshalling through a global instance,
            // COM instances that are actually CCWs should be unwrapped to the original managed object
            // to allow for round-tripping object -> COM instance -> object.
            if (extObjCxt == NULL && scenario == ComWrappersScenario::MarshallingGlobalInstance)
            {
                // If the COM instance is a CCW that is not COM-activated, use the object of that wrapper object.
                InteropLib::OBJECTHANDLE handleLocal;
                if (InteropLib::Com::GetObjectForWrapper(identity, &handleLocal) ==  S_OK
                    && InteropLib::Com::IsComActivated(identity) == S_FALSE)
                {
                    handle = handleLocal;
                }
            }
        }

        if (extObjCxt != NULL)
        {
            gc.objRefMaybe = extObjCxt->GetObjectRef();
        }
        else if (handle != NULL)
        {
            // We have an object handle from the COM instance which is a CCW. Use that object.
            // This allows for the round-trip from object -> COM instance -> object.
            ::OBJECTHANDLE objectHandle = static_cast<::OBJECTHANDLE>(handle);
            gc.objRefMaybe = ObjectFromHandle(objectHandle);
        }
        else
        {
            // Create context instance for the possibly new external object.
            ExternalWrapperResultHolder resultHolder;

            {
                GCX_PREEMP();
                hr = InteropLib::Com::CreateWrapperForExternal(
                    identity,
                    flags,
                    sizeof(ExternalObjectContext),
                    &resultHolder);
            }

            if (FAILED(hr))
                COMPlusThrowHR(hr);

            // The user could have supplied a wrapper so assign that now.
            gc.objRefMaybe = gc.wrapperMaybeRef;

            // If the wrapper hasn't been set yet, call the implementation to create one.
            if (gc.objRefMaybe == NULL)
            {
                gc.objRefMaybe = CallCreateObject(scenario, &gc.implRef, identity, flags);
            }

            // The object may be null if the specified ComWrapper implementation returns null
            // or there is no registered global instance. It is the caller's responsibility
            // to handle this case and error if necessary.
            if (gc.objRefMaybe != NULL)
            {
                // Construct the new context with the object details.
                DWORD flags = (resultHolder.Result.FromTrackerRuntime
                                ? ExternalObjectContext::Flags_ReferenceTracker
                                : ExternalObjectContext::Flags_None) |
                            (uniqueInstance
                                ? ExternalObjectContext::Flags_None
                                : ExternalObjectContext::Flags_InCache);
                ExternalObjectContext::Construct(
                    resultHolder.GetContext(),
                    identity,
                    GetCurrentCtxCookie(),
                    gc.objRefMaybe->GetSyncBlockIndex(),
                    flags);

                if (uniqueInstance)
                {
                    extObjCxt = resultHolder.GetContext();
                }
                else
                {
                    // Attempt to insert the new context into the cache.
                    ExtObjCxtCache::LockHolder lock(cache);
                    extObjCxt = cache->FindOrAdd(identity, resultHolder.GetContext());
                }

                // If the returned context matches the new context it means the
                // new context was inserted or a unique instance was requested.
                if (extObjCxt == resultHolder.GetContext())
                {
                    // Update the object's SyncBlock with a handle to the context for runtime cleanup.
                    SyncBlock* syncBlock = gc.objRefMaybe->GetSyncBlock();
                    InteropSyncBlockInfo* interopInfo = syncBlock->GetInteropInfo();
                    _ASSERTE(syncBlock->IsPrecious());

                    // Since the caller has the option of providing a wrapper, it is
                    // possible the supplied wrapper already has an associated external
                    // object and an object can only be associated with one external object.
                    if (!interopInfo->TrySetExternalComObjectContext((void**)extObjCxt))
                    {
                        // Failed to set the context; one must already exist.
                        // Remove from the cache above as well.
                        ExtObjCxtCache::LockHolder lock(cache);
                        cache->Remove(resultHolder.GetContext());

                        COMPlusThrow(kNotSupportedException);
                    }

                    // Detach from the holder to avoid cleanup.
                    (void)resultHolder.DetachContext();
                    STRESS_LOG2(LF_INTEROP, LL_INFO100, "Created EOC (Unique Instance: %d): 0x%p\n", (int)uniqueInstance, extObjCxt);
                }

                _ASSERTE(extObjCxt->IsActive());
            }
        }

        GCPROTECT_END();

        *objRef = gc.objRefMaybe;
        RETURN (gc.objRefMaybe != NULL);
    }
}

namespace InteropLibImports
{
    void* MemAlloc(_In_ size_t sizeInBytes, _In_ AllocScenario scenario) noexcept
    {
        CONTRACTL
        {
            NOTHROW;
            GC_NOTRIGGER;
            MODE_ANY;
            PRECONDITION(sizeInBytes != 0);
        }
        CONTRACTL_END;

        return ::malloc(sizeInBytes);
    }

    void MemFree(_In_ void* mem, _In_ AllocScenario scenario) noexcept
    {
        CONTRACTL
        {
            NOTHROW;
            GC_NOTRIGGER;
            MODE_ANY;
            PRECONDITION(mem != NULL);
        }
        CONTRACTL_END;

        ::free(mem);
    }

    HRESULT AddMemoryPressureForExternal(_In_ UINT64 memoryInBytes) noexcept
    {
        CONTRACTL
        {
            NOTHROW;
            MODE_PREEMPTIVE;
        }
        CONTRACTL_END;

        HRESULT hr = S_OK;
        BEGIN_EXTERNAL_ENTRYPOINT(&hr)
        {
            GCInterface::NewAddMemoryPressure(memoryInBytes);
        }
        END_EXTERNAL_ENTRYPOINT;

        return hr;
    }

    HRESULT RemoveMemoryPressureForExternal(_In_ UINT64 memoryInBytes) noexcept
    {
        CONTRACTL
        {
            NOTHROW;
            MODE_PREEMPTIVE;
        }
        CONTRACTL_END;

        HRESULT hr = S_OK;
        BEGIN_EXTERNAL_ENTRYPOINT(&hr)
        {
            GCInterface::NewRemoveMemoryPressure(memoryInBytes);
        }
        END_EXTERNAL_ENTRYPOINT;

        return hr;
    }

    HRESULT RequestGarbageCollectionForExternal(_In_ GcRequest req) noexcept
    {
        CONTRACTL
        {
            NOTHROW;
            MODE_PREEMPTIVE;
        }
        CONTRACTL_END;

        HRESULT hr = S_OK;
        BEGIN_EXTERNAL_ENTRYPOINT(&hr)
        {
            GCX_COOP_THREAD_EXISTS(GET_THREAD());
            if (req == GcRequest::FullBlocking)
            {
                GCHeapUtilities::GetGCHeap()->GarbageCollect(2, true, collection_blocking | collection_optimized);
            }
            else
            {
                _ASSERTE(req == GcRequest::Default);
                GCHeapUtilities::GetGCHeap()->GarbageCollect();
            }
        }
        END_EXTERNAL_ENTRYPOINT;

        return hr;
    }

    HRESULT WaitForRuntimeFinalizerForExternal() noexcept
    {
        CONTRACTL
        {
            NOTHROW;
            MODE_PREEMPTIVE;
        }
        CONTRACTL_END;

        HRESULT hr = S_OK;
        BEGIN_EXTERNAL_ENTRYPOINT(&hr)
        {
            FinalizerThread::FinalizerThreadWait();
        }
        END_EXTERNAL_ENTRYPOINT;

        return hr;
    }

    HRESULT ReleaseExternalObjectsFromCurrentThread() noexcept
    {
        CONTRACTL
        {
            NOTHROW;
            MODE_PREEMPTIVE;
        }
        CONTRACTL_END;

        HRESULT hr = S_OK;
        BEGIN_EXTERNAL_ENTRYPOINT(&hr)
        {
            // Switch to cooperative mode so the cache can be queried.
            GCX_COOP();

            struct
            {
                OBJECTREF implRef;
                OBJECTREF objsEnumRef;
            } gc;
            ::ZeroMemory(&gc, sizeof(gc));
            GCPROTECT_BEGIN(gc);

            gc.implRef = NULL; // Use the globally registered implementation.

            // Pass the objects along to get released.
            ExtObjCxtCache* cache = ExtObjCxtCache::GetInstanceNoThrow();
            gc.objsEnumRef = cache->CreateManagedEnumerable(
                ExternalObjectContext::Flags_ReferenceTracker,
                GetCurrentCtxCookie());

            CallReleaseObjects(&gc.implRef, &gc.objsEnumRef);

            GCPROTECT_END();
        }
        END_EXTERNAL_ENTRYPOINT;

        return hr;
    }

    void DeleteObjectInstanceHandle(_In_ InteropLib::OBJECTHANDLE handle) noexcept
    {
        CONTRACTL
        {
            NOTHROW;
            GC_NOTRIGGER;
            MODE_ANY;
            PRECONDITION(handle != NULL);
        }
        CONTRACTL_END;

        DestroyHandleCommon(static_cast<::OBJECTHANDLE>(handle), InstanceHandleType);
    }

    bool GetGlobalPeggingState() noexcept
    {
        CONTRACTL
        {
            NOTHROW;
            GC_NOTRIGGER;
            MODE_ANY;
        }
        CONTRACTL_END;

        return (RCWWalker::s_bIsGlobalPeggingOn != FALSE);
    }

    void SetGlobalPeggingState(_In_ bool state) noexcept
    {
        CONTRACTL
        {
            NOTHROW;
            GC_NOTRIGGER;
            MODE_ANY;
        }
        CONTRACTL_END;

        BOOL newState = state ? TRUE : FALSE;
        VolatileStore(&RCWWalker::s_bIsGlobalPeggingOn, newState);
    }

    HRESULT GetOrCreateTrackerTargetForExternal(
        _In_ IUnknown* externalComObject,
        _In_ CreateObjectFlags externalObjectFlags,
        _In_ CreateComInterfaceFlags trackerTargetFlags,
        _Outptr_ void** trackerTarget) noexcept
    {
        CONTRACTL
        {
            NOTHROW;
            MODE_PREEMPTIVE;
            PRECONDITION(externalComObject != NULL);
            PRECONDITION(trackerTarget != NULL);
        }
        CONTRACTL_END;

        HRESULT hr = S_OK;
        BEGIN_EXTERNAL_ENTRYPOINT(&hr)
        {
            // Switch to Cooperative mode since object references
            // are being manipulated.
            GCX_COOP();

            struct
            {
                OBJECTREF implRef;
                OBJECTREF wrapperMaybeRef;
                OBJECTREF objRef;
            } gc;
            ::ZeroMemory(&gc, sizeof(gc));
            GCPROTECT_BEGIN(gc);

            gc.implRef = NULL; // Use the globally registered implementation.
            gc.wrapperMaybeRef = NULL; // No supplied wrapper here.

            // Get wrapper for external object
            bool success = TryGetOrCreateObjectForComInstanceInternal(
                gc.implRef,
                externalComObject,
                externalObjectFlags,
                ComWrappersScenario::TrackerSupportGlobalInstance,
                gc.wrapperMaybeRef,
                &gc.objRef);

            if (!success)
                COMPlusThrow(kArgumentNullException);

            // Get wrapper for managed object
            success = TryGetOrCreateComInterfaceForObjectInternal(
                gc.implRef,
                gc.objRef,
                trackerTargetFlags,
                ComWrappersScenario::TrackerSupportGlobalInstance,
                trackerTarget);

            if (!success)
                COMPlusThrow(kArgumentException);

            STRESS_LOG2(LF_INTEROP, LL_INFO100, "Created Target for External: 0x%p => 0x%p\n", OBJECTREFToObject(gc.objRef), *trackerTarget);
            GCPROTECT_END();
        }
        END_EXTERNAL_ENTRYPOINT;

        return hr;
    }

    TryInvokeICustomQueryInterfaceResult TryInvokeICustomQueryInterface(
        _In_ InteropLib::OBJECTHANDLE handle,
        _In_ REFGUID iid,
        _Outptr_result_maybenull_ void** obj) noexcept
    {
        CONTRACTL
        {
            NOTHROW;
            MODE_ANY;
            PRECONDITION(handle != NULL);
            PRECONDITION(obj != NULL);
        }
        CONTRACTL_END;

        *obj = NULL;

        // If this is a GC thread, then someone is trying to query for something
        // at a time when we can't run managed code.
        if (IsGCThread())
            return TryInvokeICustomQueryInterfaceResult::OnGCThread;

        // Ideally the BEGIN_EXTERNAL_ENTRYPOINT/END_EXTERNAL_ENTRYPOINT pairs
        // would be used here. However, this code path can be entered from within
        // and from outside the runtime.
        MAKE_CURRENT_THREAD_AVAILABLE_EX(GetThreadNULLOk());
        if (CURRENT_THREAD == NULL)
        {
            CURRENT_THREAD = SetupThreadNoThrow();

            // If we failed to set up a new thread, we are going to indicate
            // there was a general failure to invoke instead of failing fast.
            if (CURRENT_THREAD == NULL)
                return TryInvokeICustomQueryInterfaceResult::FailedToInvoke;
        }

        HRESULT hr;
        auto result = TryInvokeICustomQueryInterfaceResult::FailedToInvoke;
        EX_TRY_THREAD(CURRENT_THREAD)
        {
            // Switch to Cooperative mode since object references
            // are being manipulated.
            GCX_COOP();

            struct
            {
                OBJECTREF objRef;
            } gc;
            ::ZeroMemory(&gc, sizeof(gc));
            GCPROTECT_BEGIN(gc);

            // Get the target of the external object's reference.
            ::OBJECTHANDLE objectHandle = static_cast<::OBJECTHANDLE>(handle);
            gc.objRef = ObjectFromHandle(objectHandle);

            result = (TryInvokeICustomQueryInterfaceResult)CallICustomQueryInterface(&gc.objRef, iid, obj);

            GCPROTECT_END();
        }
        EX_CATCH_HRESULT(hr);

        // Assert valid value.
        _ASSERTE(TryInvokeICustomQueryInterfaceResult::Min <= result
            && result <= TryInvokeICustomQueryInterfaceResult::Max);

        return result;
    }

    struct RuntimeCallContext
    {
        // Iterators for all known external objects.
        ExtObjCxtCache::Iterator Curr;
        ExtObjCxtCache::Iterator End;

        // Pointer to cache used to create object references.
        ExtObjCxtRefCache* RefCache;

        RuntimeCallContext(_In_ ExtObjCxtCache* cache)
            : Curr{ cache->_hashMap.Begin() }
            , End{ cache->_hashMap.End() }
            , RefCache{ cache->GetRefCache() }
        { }
    };

    HRESULT IteratorNext(
        _In_ RuntimeCallContext* runtimeContext,
        _Outptr_result_maybenull_ void** extObjContext) noexcept
    {
        CONTRACTL
        {
            NOTHROW;
            GC_NOTRIGGER;
            MODE_COOPERATIVE;
            PRECONDITION(runtimeContext != NULL);
            PRECONDITION(extObjContext != NULL);

            // Should only be called during a GC suspension
            PRECONDITION(Debug_IsLockedViaThreadSuspension());
        }
        CONTRACTL_END;

        if (runtimeContext->Curr == runtimeContext->End)
        {
            *extObjContext = NULL;
            return S_FALSE;
        }

        ExtObjCxtCache::Element e = *runtimeContext->Curr++;
        *extObjContext = e;
        return S_OK;
    }

    HRESULT FoundReferencePath(
        _In_ RuntimeCallContext* runtimeContext,
        _In_ void* extObjContextRaw,
        _In_ InteropLib::OBJECTHANDLE handle) noexcept
    {
        CONTRACTL
        {
            NOTHROW;
            GC_NOTRIGGER;
            MODE_COOPERATIVE;
            PRECONDITION(runtimeContext != NULL);
            PRECONDITION(extObjContextRaw != NULL);
            PRECONDITION(handle != NULL);

            // Should only be called during a GC suspension
            PRECONDITION(Debug_IsLockedViaThreadSuspension());
        }
        CONTRACTL_END;

        // Get the external object's managed wrapper
        ExternalObjectContext* extObjContext = static_cast<ExternalObjectContext*>(extObjContextRaw);
        OBJECTREF source = extObjContext->GetObjectRef();

        // Get the target of the external object's reference.
        ::OBJECTHANDLE objectHandle = static_cast<::OBJECTHANDLE>(handle);
        OBJECTREF target = ObjectFromHandle(objectHandle);

        // If these point at the same object don't create a reference.
        if (source->PassiveGetSyncBlock() == target->PassiveGetSyncBlock())
            return S_FALSE;

        STRESS_LOG2(LF_INTEROP, LL_INFO1000, "Found reference path: 0x%p => 0x%p\n",
            OBJECTREFToObject(source),
            OBJECTREFToObject(target));
        return runtimeContext->RefCache->AddReferenceFromObjectToObject(source, target);
    }
}

#ifdef FEATURE_COMWRAPPERS

BOOL QCALLTYPE ComWrappersNative::TryGetOrCreateComInterfaceForObject(
    _In_ QCall::ObjectHandleOnStack comWrappersImpl,
    _In_ QCall::ObjectHandleOnStack instance,
    _In_ INT32 flags,
    _Outptr_ void** wrapper)
{
    QCALL_CONTRACT;

    bool success;

    BEGIN_QCALL;

    // Switch to Cooperative mode since object references
    // are being manipulated.
    {
        GCX_COOP();
        success = TryGetOrCreateComInterfaceForObjectInternal(
            ObjectToOBJECTREF(*comWrappersImpl.m_ppObject),
            ObjectToOBJECTREF(*instance.m_ppObject),
            (CreateComInterfaceFlags)flags,
            ComWrappersScenario::Instance,
            wrapper);
    }

    END_QCALL;

    return (success ? TRUE : FALSE);
}

BOOL QCALLTYPE ComWrappersNative::TryGetOrCreateObjectForComInstance(
    _In_ QCall::ObjectHandleOnStack comWrappersImpl,
    _In_ void* ext,
    _In_ INT32 flags,
    _In_ QCall::ObjectHandleOnStack wrapperMaybe,
    _Inout_ QCall::ObjectHandleOnStack retValue)
{
    QCALL_CONTRACT;

    _ASSERTE(ext != NULL);

    bool success;

    BEGIN_QCALL;

    HRESULT hr;
    IUnknown* externalComObject = reinterpret_cast<IUnknown*>(ext);

    // Determine the true identity of the object
    SafeComHolder<IUnknown> identity;
    hr = externalComObject->QueryInterface(IID_IUnknown, &identity);
    _ASSERTE(hr == S_OK);

    // Switch to Cooperative mode since object references
    // are being manipulated.
    {
        GCX_COOP();

        OBJECTREF newObj;
        success = TryGetOrCreateObjectForComInstanceInternal(
            ObjectToOBJECTREF(*comWrappersImpl.m_ppObject),
            identity,
            (CreateObjectFlags)flags,
            ComWrappersScenario::Instance,
            ObjectToOBJECTREF(*wrapperMaybe.m_ppObject),
            &newObj);

        // Set the return value
        if (success)
            retValue.Set(newObj);
    }

    END_QCALL;

    return (success ? TRUE : FALSE);
}

void QCALLTYPE ComWrappersNative::GetIUnknownImpl(
        _Out_ void** fpQueryInterface,
        _Out_ void** fpAddRef,
        _Out_ void** fpRelease)
{
    QCALL_CONTRACT;

    _ASSERTE(fpQueryInterface != NULL);
    _ASSERTE(fpAddRef != NULL);
    _ASSERTE(fpRelease != NULL);

    BEGIN_QCALL;

    InteropLib::Com::GetIUnknownImpl(fpQueryInterface, fpAddRef, fpRelease);

    END_QCALL;
}

void ComWrappersNative::DestroyManagedObjectComWrapper(_In_ void* wrapper)
{
    CONTRACTL
    {
        NOTHROW;
        MODE_ANY;
        PRECONDITION(wrapper != NULL);
    }
    CONTRACTL_END;

    STRESS_LOG1(LF_INTEROP, LL_INFO100, "Destroying MOW: 0x%p\n", wrapper);
    InteropLib::Com::DestroyWrapperForObject(wrapper);
}

void ComWrappersNative::DestroyExternalComObjectContext(_In_ void* contextRaw)
{
    CONTRACTL
    {
        NOTHROW;
        MODE_ANY;
        PRECONDITION(contextRaw != NULL);
    }
    CONTRACTL_END;

#ifdef _DEBUG
    ExternalObjectContext* context = static_cast<ExternalObjectContext*>(contextRaw);
    _ASSERTE(!context->IsActive());
#endif

    STRESS_LOG1(LF_INTEROP, LL_INFO100, "Destroying EOC: 0x%p\n", contextRaw);
    InteropLib::Com::DestroyWrapperForExternal(contextRaw);
}

void ComWrappersNative::MarkExternalComObjectContextCollected(_In_ void* contextRaw)
{
    CONTRACTL
    {
        NOTHROW;
        GC_NOTRIGGER;
        MODE_ANY;
        PRECONDITION(contextRaw != NULL);
        PRECONDITION(GCHeapUtilities::IsGCInProgress());
    }
    CONTRACTL_END;

    ExternalObjectContext* context = static_cast<ExternalObjectContext*>(contextRaw);
    _ASSERTE(context->IsActive());
    context->MarkCollected();

    bool inCache = context->IsSet(ExternalObjectContext::Flags_InCache);
    STRESS_LOG2(LF_INTEROP, LL_INFO100, "Mark Collected EOC (In Cache: %d): 0x%p\n", (int)inCache, contextRaw);

    // Verify the caller didn't ignore the cache during creation.
    if (inCache)
    {
        ExtObjCxtCache* cache = ExtObjCxtCache::GetInstanceNoThrow();
        cache->Remove(context);
    }
}

void ComWrappersNative::MarkWrapperAsComActivated(_In_ IUnknown* wrapperMaybe)
{
    CONTRACTL
    {
        NOTHROW;
        MODE_ANY;
        PRECONDITION(wrapperMaybe != NULL);
    }
    CONTRACTL_END;

    // The IUnknown may or may not represent a wrapper, so E_INVALIDARG is okay here.
    HRESULT hr = InteropLib::Com::MarkComActivated(wrapperMaybe);
    _ASSERTE(SUCCEEDED(hr) || hr == E_INVALIDARG);
}

void QCALLTYPE GlobalComWrappersForMarshalling::SetGlobalInstanceRegisteredForMarshalling()
{
    // QCALL contracts are not used here because the managed declaration
    // uses the SuppressGCTransition attribute

    _ASSERTE(!g_IsGlobalComWrappersRegistered);
    g_IsGlobalComWrappersRegistered = true;
}

bool GlobalComWrappersForMarshalling::TryGetOrCreateComInterfaceForObject(
    _In_ OBJECTREF instance,
    _Outptr_ void** wrapperRaw)
{
    if (!g_IsGlobalComWrappersRegistered)
        return false;

    // Switch to Cooperative mode since object references
    // are being manipulated.
    {
        GCX_COOP();

        CreateComInterfaceFlags flags = CreateComInterfaceFlags::CreateComInterfaceFlags_TrackerSupport;

        // Passing NULL as the ComWrappers implementation indicates using the globally registered instance
        return TryGetOrCreateComInterfaceForObjectInternal(
            NULL,
            instance,
            flags,
            ComWrappersScenario::MarshallingGlobalInstance,
            wrapperRaw);
    }
}

bool GlobalComWrappersForMarshalling::TryGetOrCreateObjectForComInstance(
    _In_ IUnknown* externalComObject,
    _In_ INT32 objFromComIPFlags,
    _Out_ OBJECTREF* objRef)
{
    if (!g_IsGlobalComWrappersRegistered)
        return false;

    // Determine the true identity of the object
    SafeComHolder<IUnknown> identity;
    {
        GCX_PREEMP();

        HRESULT hr = externalComObject->QueryInterface(IID_IUnknown, &identity);
        _ASSERTE(hr == S_OK);
    }

    // Switch to Cooperative mode since object references
    // are being manipulated.
    {
        GCX_COOP();

        int flags = CreateObjectFlags::CreateObjectFlags_TrackerObject;
        if ((objFromComIPFlags & ObjFromComIP::UNIQUE_OBJECT) != 0)
            flags |= CreateObjectFlags::CreateObjectFlags_UniqueInstance;

        // Passing NULL as the ComWrappers implementation indicates using the globally registered instance
        return TryGetOrCreateObjectForComInstanceInternal(
            NULL /*comWrappersImpl*/,
            identity,
            (CreateObjectFlags)flags,
            ComWrappersScenario::MarshallingGlobalInstance,
            NULL /*wrapperMaybe*/,
            objRef);
    }
}

#endif // FEATURE_COMWRAPPERS

void Interop::OnGCStarted(_In_ int nCondemnedGeneration)
{
    CONTRACTL
    {
        NOTHROW;
        GC_NOTRIGGER;
    }
    CONTRACTL_END;

#ifdef FEATURE_COMINTEROP
    //
    // Let GC detect managed/native cycles with input from jupiter
    // Jupiter will
    // 1. Report reference from RCW to CCW based on native reference in Jupiter
    // 2. Identify the subset of CCWs that needs to be rooted
    //
    // We'll build the references from RCW to CCW using
    // 1. Preallocated arrays
    // 2. Dependent handles
    //
    RCWWalker::OnGCStarted(nCondemnedGeneration);
#endif // FEATURE_COMINTEROP

#ifdef FEATURE_COMWRAPPERS
    //
    // Note that we could get nested GCStart/GCEnd calls, such as :
    // GCStart for Gen 2 background GC
    //    GCStart for Gen 0/1 foregorund GC
    //    GCEnd   for Gen 0/1 foreground GC
    //    ....
    // GCEnd for Gen 2 background GC
    //
    // The nCondemnedGeneration >= 2 check takes care of this nesting problem
    //
    // See Interop::OnGCFinished()
    if (nCondemnedGeneration >= 2)
    {
        // If no cache exists, then there is nothing to do here.
        ExtObjCxtCache* cache = ExtObjCxtCache::GetInstanceNoThrow();
        if (cache != NULL)
        {
            STRESS_LOG0(LF_INTEROP, LL_INFO10000, "Begin Reference Tracking\n");
            ExtObjCxtRefCache* refCache = cache->GetRefCache();

            // Reset the ref cache
            refCache->ResetDependentHandles();

            // Create a call context for the InteropLib.
            InteropLibImports::RuntimeCallContext cxt(cache);
            (void)InteropLib::Com::BeginExternalObjectReferenceTracking(&cxt);

            // Shrink cache and clear unused handles.
            refCache->ShrinkDependentHandles();
        }
    }
#endif // FEATURE_COMWRAPPERS
}

void Interop::OnGCFinished(_In_ int nCondemnedGeneration)
{
    CONTRACTL
    {
        NOTHROW;
        GC_NOTRIGGER;
    }
    CONTRACTL_END;

#ifdef FEATURE_COMINTEROP
    //
    // Tell Jupiter GC has finished
    //
    RCWWalker::OnGCFinished(nCondemnedGeneration);
#endif // FEATURE_COMINTEROP

#ifdef FEATURE_COMWRAPPERS
    //
    // Note that we could get nested GCStart/GCEnd calls, such as :
    // GCStart for Gen 2 background GC
    //    GCStart for Gen 0/1 foregorund GC
    //    GCEnd   for Gen 0/1 foreground GC
    //    ....
    // GCEnd for Gen 2 background GC
    //
    // The nCondemnedGeneration >= 2 check takes care of this nesting problem
    //
    // See Interop::OnGCStarted()
    if (nCondemnedGeneration >= 2)
    {
        ExtObjCxtCache* cache = ExtObjCxtCache::GetInstanceNoThrow();
        if (cache != NULL)
        {
            (void)InteropLib::Com::EndExternalObjectReferenceTracking();
            STRESS_LOG0(LF_INTEROP, LL_INFO10000, "End Reference Tracking\n");
        }
    }
#endif // FEATURE_COMWRAPPERS
}
