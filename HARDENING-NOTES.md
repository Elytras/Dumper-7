# Dumper-7 hardening

Goal: no layout field is read without a finder or a logged, version-gated default. A wrong offset should degrade
the dump, never crash the game. Every offset is logged with how it was found.

## State (2026-10-18)
Verified by full dumps of DRG (UE 4.27, CL 154534) and RogueCore (UE 5.6, CL 158178):
- **DRG**: the native-module SDK files are unchanged against the previous main-build dump. The differences come
  from the loaded mods, plus three intended changes:
  - The IDA FField layout now comes from the found offsets; the old literal was the 5.1.1+ layout.
  - The `—` comments are fixed (/utf-8).
  - `FOptionalProperty` has no member, because DRG has no OptionalProperty.
- **RC**: native-module and core SDK files are identical to the main-build dump from the same session. The only
  differences are Blueprint function order and transient property bags, which vary from run to run.
- The offset table (`Offsets.txt` beside each dump, plus the `[offset]` lines in `Dumper-7.log`) came out the same
  across the three DRG runs.
  - **DRG**: 89 scan, 5 default, 5 missing (the UE5-shaped FScriptMapHelper/FScriptSetHelper scans), 3 unused.
  - **RC**: 93 scan, 5 default, 2 missing (`FScriptSetHelper::AddElement` inlined, `FCrc::MemCrc32` UE4-only), 2 unused.

## How it works
- `Off::Init()` (Offsets.cpp) runs the finders in dependency order and logs each result through
  `Off::Found/FoundOr/Log`.
  - It returns false when an essential offset is missing.
  - `Generator::InitEngineCore()` then stops, and main.cpp hooks and dumps nothing.
  - The essentials are listed in `Off::MissingEssentials()`. A property-kind member that the game never uses is
    logged `unused` instead of failing the dump.
- `OffsetFinder.cpp`: every finder checks its result against live objects (`IsLiveObject`, `IsPropertyOwnedBy`,
  `SampleProperties`) and returns -1 instead of a guess.
- Getters return null on a missing offset where every caller handles null. Generators drop predefined members at a
  missing offset (`DropMembersAtMissingOffsets`).
- `NameArray::IsValidCompIdx` bounds-checks an FName index before the engine's AppendString sees it. This is enabled
  only after every object name passes the check.

## Follow-ups
- The DRG Script check counts 1 of 37908 Blueprint scripts not ending in EX_EndOfScript. The check now logs such
  scripts by name (e92fb05), but this build hasn't been run on DRG yet.
- The bytecode disassembler decodes `EX_ArrayConst` operands wrongly on UE5: `inner=<unreadable>`, and huge counts
  that change from run to run.
- Emitted-SDK layout assumptions that nothing checks against the game:
  - NewObject FConstructParams uses the 4.27 shape (0x40). UE5.6's FStaticConstructObjectParameters is 0x80 on
    RogueCore CL-158178. SCO (0x15E2C30) reads PropertyInitCallback at +0x40 (a TFunction called via +0x50 or
    +0x68), SerialNumber at +0x70 and SubobjectOverrides at +0x78. A 0x40 struct hands it stack garbage. Emit a
    zero-filled 0x80 struct on 5.6.
  - The UnrealAllocator FMalloc vtable is UE4-shaped (CG:4485-4507). UE5.6 adds MallocZeroed and TryMallocZeroed,
    and MarkTLSCachesAsUsed/Unused.
  - The UClass internals past CF+0x118 are not covered by the CDO==CF+0x48 gate.
  - FUObjectItem serial +0x10 and ObjObjects 0x10 are constexpr constants.
  - The Map/Set bridge reads MapLayout and MapFlags at Base+0x10 and Base+0x28 (CG:7895).
  - FScriptDelegate changes size on UE5.3+ race-detection builds.
  - The UEnum::Names type ignores the enum-layout flags.
  - The TPersistentObjectPtr tag is assumed. FFieldPath is assumed to be at most 0x20. FFrame has Pad00.
  - TOptional's intrusive IsSet is broken.
  - The outline-number FName codegen is wrong (CG:5652, 5965-5970, 6653).
  - VTableOffsets are picked by the LWC proxy (CG:11791).
  - IsValid tests 0x40000000, so pending-kill objects pass on 4.27.
  - The fixed TUObjectArray uses `Index > NumElements` (CG:5243, 5254).
