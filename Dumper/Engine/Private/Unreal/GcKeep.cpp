#include <atomic>
#include <iostream>
#include <format>
#include <mutex>
#include <vector>
#include <algorithm>

#include "Unreal/GcKeep.h"
#include "Unreal/ObjectArray.h"
#include "Unreal/UnrealObjects.h"
#include "Platform.h"

namespace GcKeep
{
	/* ---- FReferenceCollector ABI mirrors -------------------------------------------------------------
	   We never instantiate these. We reinterpret the collector the engine hands us and call exactly ONE
	   virtual, so the only thing that has to be right is the SLOT INDEX of HandleObjectReference.

	   That index is NOT stable across engine generations, and getting it wrong is completely silent on
	   x64: the Microsoft ABI is caller-cleanup with the first four arguments in volatile registers, so a
	   call that lands on a neighbouring virtual just reads RCX, ignores the rest, and returns. No crash,
	   no stack imbalance, nothing reported to the collector - the keep degrades to a no-op that still
	   logs success. (On x86 the same mistake would corrupt the stack via __thiscall's callee-cleanup
	   `ret N`, which is at least loud. Keep that in mind if a 32-bit target is ever wired up.)

	   Slots verified against the engine headers:
	     UE4.27 CoreUObject/Public/UObject/UObjectGlobals.h - HandleObjectReference is slot 7
	     UE5.6  CoreUObject/Public/UObject/UObjectGlobals.h - slot 17; six AddStableReference* overloads
	            plus NeedsPropertyReferencer / SetIsProcessingNativeReferences /
	            IsProcessingNativeReferences / NeedsInitialReferences were inserted ahead of it.
	   Declaration order is vtable order. MSVC emits same-name overloads in reverse within their group,
	   which permutes indices INSIDE a group but never changes how many entries precede a uniquely-named
	   virtual, so the arithmetic holds regardless. Signatures are pointer-only (void* for UObject /
	   FProperty) - we need the calling convention, not the layouts.

	   Which mirror applies is not guessed: the FGCObject registration ABI resolved in Init() already
	   identifies the engine generation (V1_TwoArg = UE4-era, V2_OneArg = UE5-era), so each node type
	   below is hard-wired to its matching collector shape. */

	/* Distinct opaque stand-ins so the overload SETS match the engine's shape (TObjectPtr<UObject>*,
	   TArray<...>*, TSet<...>* etc). Never defined - only ever used as pointer parameter types. */
	struct FOpaqueObjectPtr;
	struct FOpaqueObjectPtrArray;
	struct FOpaqueObjectPtrSet;
	struct FOpaqueObjectArray;
	struct FOpaqueObjectSet;

	/* UE4-era layout (validated on UE4.27). HandleObjectReference == slot 7. */
	class FReferenceCollectorUE4
	{
	public:
		virtual ~FReferenceCollectorUE4() {}                                        // 0
		virtual bool  IsIgnoringArchetypeRef() const { return false; }              // 1
		virtual bool  IsIgnoringTransient() const { return false; }                 // 2
		virtual void  AllowEliminatingReferences(bool) {}                           // 3
		virtual void  SetSerializedProperty(void*) {}                               // 4
		virtual void* GetSerializedProperty() const { return nullptr; }             // 5
		virtual bool  MarkWeakObjectReferenceForClearing(void**) { return false; }  // 6
		virtual void  HandleObjectReference(void*& Object, const void* RefObj, const void* RefProp) = 0; // 7
		virtual void  HandleObjectReferences(void**, int32, const void*, const void*) {}                 // 8
	};

	/* UE5-era layout (validated on UE5.6). HandleObjectReference == slot 17. */
	class FReferenceCollectorUE5
	{
	public:
		virtual ~FReferenceCollectorUE5() {}                                        // 0
		virtual void  AddStableReference(FOpaqueObjectPtr*) {}                      // 1..6 (one overload
		virtual void  AddStableReferenceArray(FOpaqueObjectPtrArray*) {}            //  set per name; MSVC
		virtual void  AddStableReferenceSet(FOpaqueObjectPtrSet*) {}                //  may reverse within
		virtual void  AddStableReference(void**) {}                                 //  a name, count is
		virtual void  AddStableReferenceArray(FOpaqueObjectArray*) {}               //  what matters)
		virtual void  AddStableReferenceSet(FOpaqueObjectSet*) {}                   //
		virtual bool  NeedsPropertyReferencer() const { return true; }              // 7
		virtual bool  IsIgnoringArchetypeRef() const { return false; }              // 8
		virtual bool  IsIgnoringTransient() const { return false; }                 // 9
		virtual void  AllowEliminatingReferences(bool) {}                           // 10
		virtual void  SetSerializedProperty(void*) {}                               // 11
		virtual void* GetSerializedProperty() const { return nullptr; }             // 12
		virtual bool  MarkWeakObjectReferenceForClearing(void**, void*) { return false; } // 13 (gained an arg in UE5)
		virtual void  SetIsProcessingNativeReferences(bool) {}                      // 14
		virtual bool  IsProcessingNativeReferences() const { return true; }         // 15
		virtual bool  NeedsInitialReferences() const { return true; }               // 16
		virtual void  HandleObjectReference(void*& Object, const void* RefObj, const void* RefProp) = 0; // 17
	};

	/* Stand-in for a by-value FString return (TArray<TCHAR>: Data ptr + Num + Max on every engine
	   version we target). GetReferencerName is leak-diagnostics only, essentially never called on the
	   GC hot path - this only needs to be ABI-compatible, not meaningful. */
	struct DummyFString
	{
		void* Data = nullptr;
		int32 Num = 0;
		int32 Max = 0;
	};

	/* Proof-of-life counters. The failure this code shipped with was invisible precisely because
	   registration succeeded and the callback fired - only the reported reference went nowhere. These
	   make the end-to-end path measurable (see GcKeep::Stats, logged at dump time): AroCalls == 0 after
	   a GC means registration is broken; AroCalls > 0 with objects still dying means the collector slot
	   is wrong again. Plain counters, no I/O - this runs inside a GC pass. */
	namespace detail
	{
		inline std::atomic<uint64_t> AroCalls{ 0 };
		inline std::atomic<uint64_t> ObjectsReported{ 0 };
	}

	/* FGCObject vtable (identical on UE4.27 and UE5.6):
	     0 ~FGCObject | 1 AddReferencedObjects | 2 GetReferencerName | 3 GetReferencerPropertyName
	   All four are declared so a call into slot 3 lands on a real function instead of running off the
	   end of our vtable. */

	/* V1 (UE4-era) node: {vtable; Kept}. UGCObjectReferencer::AddObject stores the node pointer itself
	   into its array, so all we need to report is the kept object. */
	struct NodeV1
	{
		void* Kept = nullptr;
		explicit NodeV1(void* InKept) : Kept(InKept) {}
		virtual ~NodeV1() {}
		virtual void AddReferencedObjects(FReferenceCollectorUE4& Collector)
		{
			detail::AroCalls.fetch_add(1, std::memory_order_relaxed);

			void* Obj = Kept;
			if (Obj)
			{
				Collector.HandleObjectReference(Obj, nullptr, nullptr);
				detail::ObjectsReported.fetch_add(1, std::memory_order_relaxed);
			}
		}
		virtual DummyFString GetReferencerName() const { return DummyFString{}; }
		virtual bool GetReferencerPropertyName(void*, DummyFString&) const { return false; }
	};

	/* V2 (UE5-era) node: FGCObject itself now carries {bWeak; bReferenceAdded} right after the vtable -
	   the native AddObject/RemoveObject read/write those two bytes at fixed offsets +8/+9 on the node
	   we pass in, so they MUST be the first two members here (matches the real engine's FGCObject
	   layout for these builds). */
	struct NodeV2
	{
		bool bWeak = false;
		bool bReferenceAdded = false;
		void* Kept = nullptr;
		explicit NodeV2(void* InKept) : Kept(InKept) {}
		virtual ~NodeV2() {}
		virtual void AddReferencedObjects(FReferenceCollectorUE5& Collector)
		{
			detail::AroCalls.fetch_add(1, std::memory_order_relaxed);

			void* Obj = Kept;
			if (Obj)
			{
				Collector.HandleObjectReference(Obj, nullptr, nullptr);
				detail::ObjectsReported.fetch_add(1, std::memory_order_relaxed);
			}
		}
		virtual DummyFString GetReferencerName() const { return DummyFString{}; }
		virtual bool GetReferencerPropertyName(void*, DummyFString&) const { return false; }
	};

	namespace detail
	{
		enum class EAbi { None, V1_TwoArg, V2_OneArg };

		inline EAbi  Abi = EAbi::None;
		inline void* ReferencerObj = nullptr; // V1 only: the live UGCObjectReferencer this-pointer
		inline void* AddFn = nullptr;
		inline void* RemFn = nullptr;
		inline bool  bInitDone = false;

		inline std::mutex& Mx() { static std::mutex M; return M; }
		inline std::vector<void*>& Registry() { static std::vector<void*> V; return V; }

		using FnAddV1 = void(__fastcall*)(void* Referencer, void* Node);
		using FnRemV1 = void(__fastcall*)(void* Referencer, void* Node);
		using FnAddV2 = void(__fastcall*)(void* Node);
		using FnRemV2 = void(__fastcall*)(void* Node);

		/* Full-prologue signatures (through the EnterCriticalSection call / equivalent), reverse
		   engineered and validated live against the two engines this project targets:
		     DRG    = Deep Rock Galactic,  UE4.27 shipping (FSD-Win64-Shipping.exe)
		     RC     = RogueCore,           UE5.6 shipping  (RogueCore-Win64-Shipping.exe)
		   Wildcarded bytes are rip-relative displacements (import thunks / globals), which differ per
		   build even on an identical engine version. If a given build's compiler output doesn't match
		   either shape (different engine minor version, different compiler), FindPattern simply returns
		   null and GcKeep stays unavailable - callers must handle that, nothing here guesses. */
		constexpr const char* DrgAddObjectSig =
			"48 89 5C 24 08 48 89 6C 24 10 48 89 74 24 18 57 48 83 EC 20 "
			"48 8D 59 38 48 8B F9 48 8B CB 48 8B EA FF 15 ? ? ? ?";

		constexpr const char* DrgRemoveObjectSig =
			"48 89 5C 24 08 48 89 74 24 10 57 48 83 EC 20 "
			"48 8D 59 38 48 8B F9 48 8B CB 48 8B F2 FF 15 ? ? ? ?";

		/* RC's shared "unregister from GCObjectReferencer" helper - called from ~60 real FGCObject-
		   derived destructors (FSlateDrawBuffer, FDeferredCleanupSlateBrush, FRepLayout, ...). Takes the
		   node itself; reads the live referencer global (rip-rel, wildcarded) and the node's own
		   bWeak/bReferenceAdded bytes at +8/+9. */
		constexpr const char* RcRemoveObjectSig =
			"40 57 48 83 EC 20 48 8B 05 ? ? ? ? 48 8B F9 48 85 C0 74 61 80 79 09 00 74 5B";

		/* RC's paired "register with GCObjectReferencer" helper, called from real FGCObject-derived
		   constructors (FSlateDrawBuffer::FSlateDrawBuffer, SObjectWidget::SObjectWidget, FRepLayout::
		   FRepLayout, ...). Distinctive /GS-protected prologue: exact 0xB0 stack frame + cookie stash
		   offset + the lazy-init guard read right after. */
		constexpr const char* RcAddObjectSig =
			"4C 8B DC 57 48 81 EC B0 00 00 00 48 8B 05 ? ? ? ? "
			"48 33 C4 48 89 84 24 A0 00 00 00 80 3D ? ? ? ? 00 48 8B F9";
	}

	bool IsAvailable()
	{
		return detail::Abi != detail::EAbi::None;
	}

	FStats Stats()
	{
		FStats S;
		S.AroCalls = detail::AroCalls.load(std::memory_order_relaxed);
		S.ObjectsReported = detail::ObjectsReported.load(std::memory_order_relaxed);
		{
			std::lock_guard<std::mutex> Lock(detail::Mx());
			S.LiveNodes = detail::Registry().size();
		}
		return S;
	}

	void Init()
	{
		if (detail::bInitDone)
			return;

		detail::bInitDone = true;

		/* The live UGCObjectReferencer instance is a normal reflected UObject (UCLASS "GCObjectReferencer")
		   sitting in GObjects - no RVA/global-pointer needed, just find it. Only used by the V1 ABI; V2's
		   native calls resolve their own referencer internally. */
		for (UEObject Obj : ObjectArray())
		{
			if (!Obj)
				continue;

			UEClass Cls = Obj.GetClass();
			if (Cls && Cls.GetName() == "GCObjectReferencer")
			{
				detail::ReferencerObj = Obj.GetAddress();
				break;
			}
		}

		if (void* Add = Platform::FindPattern(detail::DrgAddObjectSig))
		{
			if (void* Rem = Platform::FindPattern(detail::DrgRemoveObjectSig))
			{
				detail::AddFn = Add;
				detail::RemFn = Rem;
				detail::Abi = detail::EAbi::V1_TwoArg;
			}
		}

		if (detail::Abi == detail::EAbi::None)
		{
			if (void* Add = Platform::FindPattern(detail::RcAddObjectSig))
			{
				if (void* Rem = Platform::FindPattern(detail::RcRemoveObjectSig))
				{
					detail::AddFn = Add;
					detail::RemFn = Rem;
					detail::Abi = detail::EAbi::V2_OneArg;
				}
			}
		}

		if (detail::Abi == detail::EAbi::V1_TwoArg && !detail::ReferencerObj)
		{
			// Found the native calls but the referencer instance isn't up yet - too early to use safely.
			detail::Abi = detail::EAbi::None;
		}

		if (IsAvailable())
		{
			std::cerr << std::format("[Dumper-7] GcKeep: resolved native FGCObject keep ({}).\n",
				detail::Abi == detail::EAbi::V1_TwoArg ? "UE4-style" : "UE5-style");
		}
		else
		{
			std::cerr << "[Dumper-7] GcKeep: could not resolve a native FGCObject keep for this build - unavailable.\n";
		}
	}

	void* Keep(void* Object)
	{
		if (!Object || !IsAvailable())
			return nullptr;

		if (detail::Abi == detail::EAbi::V1_TwoArg)
		{
			if (!detail::ReferencerObj)
				return nullptr;

			NodeV1* Node = new NodeV1(Object);
			reinterpret_cast<detail::FnAddV1>(detail::AddFn)(detail::ReferencerObj, Node);

			std::lock_guard<std::mutex> Lock(detail::Mx());
			detail::Registry().push_back(Node);
			return Node;
		}
		else
		{
			NodeV2* Node = new NodeV2(Object);
			reinterpret_cast<detail::FnAddV2>(detail::AddFn)(Node);

			std::lock_guard<std::mutex> Lock(detail::Mx());
			detail::Registry().push_back(Node);
			return Node;
		}
	}

	void Release(void* Handle)
	{
		if (!Handle || !IsAvailable())
			return;

		{
			std::lock_guard<std::mutex> Lock(detail::Mx());
			auto& Reg = detail::Registry();
			auto It = std::find(Reg.begin(), Reg.end(), Handle);
			if (It == Reg.end())
				return; // already released (or never registered)
			Reg.erase(It);
		}

		if (detail::Abi == detail::EAbi::V1_TwoArg)
		{
			if (detail::ReferencerObj)
				reinterpret_cast<detail::FnRemV1>(detail::RemFn)(detail::ReferencerObj, Handle);
			delete static_cast<NodeV1*>(Handle);
		}
		else
		{
			reinterpret_cast<detail::FnRemV2>(detail::RemFn)(Handle);
			delete static_cast<NodeV2*>(Handle);
		}
	}

	void ReleaseAll()
	{
		std::vector<void*> Nodes;
		{
			std::lock_guard<std::mutex> Lock(detail::Mx());
			Nodes.swap(detail::Registry());
		}

		if (!IsAvailable())
			return;

		for (void* Node : Nodes)
		{
			if (detail::Abi == detail::EAbi::V1_TwoArg)
			{
				if (detail::ReferencerObj)
					reinterpret_cast<detail::FnRemV1>(detail::RemFn)(detail::ReferencerObj, Node);
				delete static_cast<NodeV1*>(Node);
			}
			else
			{
				reinterpret_cast<detail::FnRemV2>(detail::RemFn)(Node);
				delete static_cast<NodeV2*>(Node);
			}
		}
	}

	ClassKeep KeepClass(void* Cls, void* Package, void* Cdo)
	{
		ClassKeep K;
		if (Cls)     K.Cls = Keep(Cls);
		if (Package) K.Pkg = Keep(Package);
		if (Cdo)     K.Cdo = Keep(Cdo);
		return K;
	}

	void ReleaseClass(ClassKeep& K)
	{
		Release(K.Cls);
		Release(K.Pkg);
		Release(K.Cdo);
		K = ClassKeep{};
	}
}
