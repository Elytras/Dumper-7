# Dumper-7 hardening: work in progress

Branch `claude/heuristic-germain-e69887`, on main 36830c0. **Not merged.** Steps 1-5 below are done and the
branch builds (`modcli build(target="dumper")`, with a copy of the untracked `build.ps1` in the worktree root).
**Nothing has been verified by a dump yet. Next: step 6.**

Goal: no layout field is read without a finder or a logged, version-gated default. A wrong offset should degrade
the dump, never crash the game. Every offset is logged with how it was found.

## Written, not verified
- `OffsetFinder.cpp`: finders that check their result against live objects. They cover UField::Next, FField
  Class/Owner/Next/Flags, FFieldClass CastFlags/Id/SuperClass, Script, the property-kind members
  (Object/Class/Struct/Delegate/Multicast/Interface/Inner/Set/Map/Enum/FieldPath/Optional), RepIndex/BRC,
  RepNotifyFunc (compares raw name indices), the NextRef/DestructorLink/PostConstructLink chains, DataTable RowMap and
  FUObjectItem::Flags. They also include `IsLiveObject` and `IsPropertyOwnedBy`. A finder that fails returns -1
  instead of a guess.
- `ObjectArray`: `GetItemAddress`; `Index >= Num()` bounds; the GC keep sets RootSet only once FUObjectItem::Flags is confirmed.
- `NameArray` + `UnrealTypes.cpp`: CompIdx bounds check, turned on only after every object name passes;
  `FName::ToRawWString` refuses out-of-table indices (`InvalidName_X`).
- `Offsets.cpp`: the offset table (`Off::Log/Found/FoundOr/FoundRva/MissingEssentials/PrintSummary/WriteTable`),
  with InitTextOffsets guarded and logged.
- `Offsets.h` / `OffsetFinder.h`: `EOffsetSource::Unused`, `bool Off::Init()`, and the new finder signatures.

## Next steps, in order
1. **OffsetFinder.cpp**: make `SampleProperties` the non-static `OffsetFinder::SampleProperties`.
   `FindFFieldNameOffset`: loop from `Off::FField::Next + 8` and drop the pre-check at the default slot, so the
   owner/next pointers are never read as names. `FindEnumNamesOffset`: pass a non-null enum to
   `InializeUEnumSettings`. `FindLevelActorsOffset`: accept an array only if its first non-null element is a live
   Actor. `FindImplementedInterfacesOffset`: add null guards.
2. **Offsets.cpp**: replace `Off::Init` with the draft, revised as follows:
   - Only `UObject::Flags` keeps a stock default (gated on Index == 0xC). Class, Outer and Name use plain `Found`,
     because they are dereferenced.
   - Return early when `FField::Class` is missing.
   - A `KindMember` lambda: a missing offset whose kind has no property in the game is logged as `Unused`.
   - `RowMap` uses plain `Found`.
   - `PropertySizes::Init` becomes one `Measure` lambda. Delete the three old functions: the multicast fallback
     wrote `DelegateProperty`, which is a bug.
   - Handle `Unused` in SourceName/FormatEntry/PrintSummary/WriteTable.
   - Add to `MissingEssentials`: FFieldClass::Name, ObjectProperty::PropertyClass, StructProperty::Struct,
     ArrayProperty::Inner, SetProperty::ElementProp, MapProperty::Base, EnumProperty::Base,
     OptionalProperty::ValueProperty. Their consumers dereference without null checks.
3. **Getters (UnrealObjects)**: return null when the offset is -1. The consumers are already null-safe.
   - `GetExecFunction`, `UEByteProperty::GetEnum`, `UEClassProperty::GetMetaClass` (and its GetCppType).
   - Delegate and Multicast `GetSignatureFunction`. Multicast must read `Off::MulticastDelegateProperty::SignatureFunction`.
   - `UEFieldPathProperty::GetFieldClass`, falling back to `FField` (also CppGenerator.cpp:1487).
   - A new `UEInterfaceProperty::GetPropertyClass` that reads `Off::InterfaceProperty::InterfaceClass`.
   - `UEEnum::GetNameValuePairs`: return a logged empty result instead of `Sleep` + `exit(1)`.
4. **Generators**:
   - Add `DropMembersAtMissingOffsets` to PredefinedMembers.h: erase each non-static, non-zero-size member at
     offset -1. Call it in CppGenerator's `SortMembers` and in `IDAMappingGenerator::InitPredefinedMembers`.
   - Sort the Interface and Enum member vectors as well.
   - Use `OffsetPlus` for Map ValueProperty and Enum Enum (Base+8).
   - Give `FMulticastDelegateProperty` its own members.
   - SharedPredefinedMembers: add Level/DataTable null guards and skip RowMap when it is -1.
   - IDA FField predef: build it from `Off::FField::*`. The current literal is the 5.1.1+ layout and is wrong on 4.27.
     Add a null CDO check at :743 and skip a null ExecFunction at :811.
   - MemberWrappers:289: return 0 for a null ExecFunction.
   - BytecodeGenerator: read `Off::UStruct::Script` instead of the literal 0x60.
5. **`Generator::InitEngineCore` returns bool**:
   - Stop when `!Off::Init()` or `MissingEssentials` is non-empty. Do this before PropertySizes and InitPE.
   - Log every auxiliary RVA with `FoundRva`, then call PrintSummary.
   - `Generate<T>` writes `Offsets.txt`.
   - main.cpp skips GcKeep, the hook and the dump when it returns false.
6. **Build and verify**: build with modcli `build(target="dumper")`. Dump DRG (4.27) and RC (5.6) in a slot, and
   diff each SDK against `C:\Dumper-7\4.27.2-154534+main-FSD` and `5.6.1-158178+main-RogueCore`. Merge into main only after that.

## Emitted-SDK layout assumptions (inventory, not yet addressed)
These assumptions are not checked against the game:
- **NewObject FConstructParams** uses the 4.27 shape (0x40, ends at ExternalPackage) (CG:2969-2981). UE5.6's
  FStaticConstructObjectParameters is 0x80 on RogueCore CL-158178. SCO (0x15E2C30) reads PropertyInitCallback at
  +0x40, a TFunction called via +0x50 or +0x68, then SerialNumber at +0x70 and SubobjectOverrides at +0x78. A 0x40
  struct hands it stack garbage; the mod's own copy crashed exactly that way. Emit a zero-filled 0x80 struct on 5.6.
- The UnrealAllocator FMalloc vtable is UE4-shaped (CG:4485-4507). UE5.6 adds MallocZeroed and TryMallocZeroed, and
  MarkTLSCachesAsUsed/Unused.
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
