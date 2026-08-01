#pragma once
/*
GcKeep.h - keep a UObject alive via UE's real FGCObject mechanism (robust; beats RootSet).

RootSet/AddToRoot is a FLAG the GC (and other engine unload paths, e.g. BP hot-reload/class-trash) can
still bypass. FGCObject instead makes the object a GENUINE reference: the engine's rooted
UGCObjectReferencer singleton calls every registered FGCObject's AddReferencedObjects each GC pass, and
we report our kept object back -> it's truly reachable.

We don't link against the real engine, so we replicate FGCObject's/FReferenceCollector's vtable ABI
(exact virtual order -> the compiler computes the right slots) and call the engine's own native
Add/RemoveObject by address - everything here is auto-resolved at runtime (GObjects class-name scan
for the referencer instance, byte-signature scan for the two native calls), no per-game hardcoded RVAs.

Two calling conventions are supported, auto-detected by which signature matches this build:
  - V1 (UE4-era): UGCObjectReferencer::AddObject(UGCObjectReferencer* This, FGCObject* Node) - the node
    is a plain {vtable; Kept} struct, `This` is the live referencer instance found via GObjects.
  - V2 (UE5-era): a self-contained AddObject(FGCObject* Node) that resolves the referencer internally;
    the node itself carries two extra bytes (bWeak/bReferenceAdded) right after its vtable, read by the
    native call at fixed offsets +8/+9.
Both were reverse-engineered and validated against a live UE4.27 and a live UE5.6 build respectively
(see GcKeep.cpp for the exact signatures). If neither matches (unknown engine build), IsAvailable()
returns false and callers should fall back (e.g. to RootSet) rather than call blindly.

Node vtables live in THIS module, so ReleaseAll() MUST run before the dumper unloads (a GC pass calling
a freed vtable is a crash) - see the F6 unload handler in Main.cpp.
*/
#include <cstdint>

namespace GcKeep
{
	/* Resolves the referencer instance + native Add/RemoveObject for this engine build. Call once,
	   after ObjectArray::Init() (needs GObjects) and before the first Keep(). Safe to call multiple
	   times; only the first call does work. */
	void Init();

	/* Whether Init() found a usable native keep path for this build. */
	bool IsAvailable();

	/* Keep `Object` (a raw UObject address) alive until Release(handle). Returns an opaque handle, or
	   nullptr if unavailable / Object is null. Game thread. */
	void* Keep(void* Object);

	void Release(void* Handle);

	/* Unregister + free every outstanding node. MUST run before the module unmaps. */
	void ReleaseAll();

	/* Handles for a class kept via KeepClass - release with ReleaseClass (or let ReleaseAll cover it). */
	struct ClassKeep
	{
		void* Cls = nullptr;
		void* Pkg = nullptr;
		void* Cdo = nullptr;
	};

	/* Keep a UClass (e.g. a BlueprintGeneratedClass) permanently resident: the class object itself, its
	   owning package, and its CDO. Nothing else is touched - instances of the class still GC normally. */
	ClassKeep KeepClass(void* Cls, void* Package, void* Cdo);

	void ReleaseClass(ClassKeep& Keep);
}
