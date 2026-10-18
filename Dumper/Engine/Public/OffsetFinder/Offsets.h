#pragma once

#include <filesystem>
#include <string>
#include <vector>

#include "Unreal/Enums.h"
#include "../Settings.h"

/* How a value in the offset table was obtained. Printed beside every offset (see Off::Log), so the
   next game update's log diff shows which layouts moved and which were never checked on this build. */
enum class EOffsetSource : uint8
{
	Scan,     // found, or verified, against live objects or code of this very build
	Override, // set by hand in Generator::InitEngineCore()
	Default,  // not checkable on this build: a known value, applied only where its note says it holds
	Missing,  // not found: never read, and whatever needs it is skipped
	Unused,   // nothing in this game has it (a property kind it never uses), so there is nothing to find
};

struct FFixedUObjectArrayLayout
{
	int32 ObjectsOffset = -1;
	int32 MaxObjectsOffset = -1;
	int32 NumObjectsOffset = -1;

	inline bool IsValid() const
	{
		return ObjectsOffset != -1 && MaxObjectsOffset != -1 && NumObjectsOffset != -1;
	}
};

struct FChunkedFixedUObjectArrayLayout
{
	int32 ObjectsOffset = -1;
	int32 MaxElementsOffset = -1;
	int32 NumElementsOffset = -1;
	int32 MaxChunksOffset = -1;
	int32 NumChunksOffset = -1;

	inline bool IsValid() const
	{
		return ObjectsOffset != -1 && MaxElementsOffset != -1 && NumElementsOffset != -1 && MaxChunksOffset != -1 && NumChunksOffset != -1;
	}
};

namespace Off
{
	/* Finds and logs every offset below. False when it had to stop early: an essential offset is missing
	   and the finders after it would read through it. */
	bool Init();

	/* The offset table. Every finder result goes through one of these: they print the entry at once,
	   so a crash later in init still leaves it in Dumper-7.log, and keep it for the summary at the end
	   of Generator::InitEngineCore() and for the Offsets.txt written beside each dump. Each returns the
	   value to store, so a call sits on the right-hand side of the assignment it logs. */
	int32 Log(const std::string& Name, int32 Value, EOffsetSource Source, const std::string& Note = {});

	/* A finder's result: Scan, or Missing when it returned OffsetFinder::OffsetNotFound. */
	int32 Found(const std::string& Name, int32 Value);

	/* A finder's result, or - only when it failed and bDefaultHolds - Default, logged with HoldsFor, the
	   engine versions it is known for. With bDefaultHolds false a failed finder stays Missing. */
	int32 FoundOr(const std::string& Name, int32 Value, int32 Default, bool bDefaultHolds, const std::string& HoldsFor);

	/* A module-relative function address from a signature scan; 0 is logged as Missing and returned as 0. */
	int32 FoundRva(const std::string& Name, int32 Rva);

	/* The essential fields (UObject/UStruct/FField/FProperty basics) that came out Missing. Without them
	   no dump is possible, so Generator::InitEngineCore() stops instead of reading at a guessed offset. */
	std::vector<std::string> MissingEssentials();

	/* Counts per source, plus every entry that is not a plain scan, printed once init is done. */
	void PrintSummary();
	void WriteTable(const std::filesystem::path& File);

	//Offsets not to be used during generation but inside of the generated SDK
	namespace InSDK
	{	
		namespace GMalloc 
		{
			inline int32 GMallocOffset = 0x0;
			inline int32 CreateGMallocOffset = 0x0;
			void InitGMalloc();
		}
		namespace ProcessEvent
		{
			inline int32 PEIndex;
			inline int32 PEOffset;

			void InitPE_Windows();
			void InitPE(const int32 Index, const char* const ModuleName = Settings::General::DefaultModuleName);
		}

		namespace World
		{
			inline int32 GWorld = 0x0;

			void InitGWorld();
		}

		namespace ObjArray
		{
			inline int32 GObjects;
			inline int32 ChunkSize;
			inline int32 FUObjectItemSize;
			inline int32 FUObjectItemInitialOffset;
		}

		namespace Name
		{
			/* Whether we're using FName::AppendString or, in an edge case, FName::ToString */
			inline bool bIsUsingAppendStringOverToString = true;
			inline bool bIsAppendStringInlinedAndUsed = false;
			inline int32 AppendNameToString;
			inline int32 GetNameEntryFromName;
			inline int32 FNameSize;

			inline int32 FNameCtorWcharOffset = 0x0; // FName::FName(const wchar_t*[, EFindName]), module-relative; 0 if not found
			void InitFNameCtorWchar();
		}

		namespace NameArray
		{
			inline int32 GNames = 0x0;
			inline int32 FNamePoolBlockOffsetBits = 0x0;
			inline int32 FNameEntryStride = 0x0;
		}

		namespace Properties
		{
			inline int32 PropertySize;
		}

		namespace Text
		{
			inline int32 TextDatOffset = 0x0;

			inline int32 InTextDataStringOffset = 0x0;

			inline int32 TextSize = 0x0;

			void InitTextOffsets();

			inline int32 FTextCtorFStringOffset = 0x0; // FText::FText(FString&&), module-relative; 0 if not found
			void InitFTextCtorFString();
		}

		namespace Engine
		{
			inline int32 UGameEngineTickOffset = 0x0; // UGameEngine::Tick, module-relative; 0 if not found
			inline int32 UGameEngineTickIndex = -1;   // UGameEngine::Tick vtable slot index; -1 if not found
			void InitUGameEngineTick();
		}

		namespace Construct
		{
			inline int32 StaticConstructObjectInternalOffset = 0x0; // StaticConstructObject_Internal, module-relative; 0 if not found
			void InitStaticConstructObjectInternal();
		}

		namespace ScriptVM
		{
			// Blueprint-VM steppers. FFrame::Step has a signature byte-identical across UE 4.27 and
			// UE 5.6 (only the GNatives lea-displacement varies), making it an exceptionally stable
			// scan; GNatives (the EX-opcode->handler dispatch table) is decoded deterministically from
			// the `lea r9, [rip+disp32]` inside FFrame::Step. StepExplicitProperty masks the
			// CPF_OutParm byte offset (r8+0x38 UE5 / r8+0x40 UE4). All module-relative; 0 if not found.
			inline int32 FFrameStepOffset = 0x0;                 // FFrame::Step
			inline int32 FFrameStepExplicitPropertyOffset = 0x0; // FFrame::StepExplicitProperty
			inline int32 GNativesOffset = 0x0;                   // GNatives (EX-opcode handler table)
			void InitScriptVM();
		}

		namespace ScriptContainers
		{
			// Reflected-container edit helpers. FScriptMapHelper::FindOrAdd/RemoveAt let a consumer add/
			// remove TMap pairs without reimplementing TSet::Add or GetTypeHash (the keyprop self-hashes).
			// The prologue unpacks the helper inline (movzx eax,[rcx+0x30]=MapFlags; reads MapLayout@+0x18,
			// KeyProp@+0, ValueProp@+8, Map@+0x10) — distinctive + stable; frame-size immediate masked.
			// UE5.6-shaped (UE4.27 has no MapFlags field). All module-relative; 0 if not found.
			inline int32 FScriptMapHelperFindOrAddOffset = 0x0;       // FScriptMapHelper::FindOrAdd
			inline int32 FScriptMapHelperRemoveAtOffset = 0x0;        // FScriptMapHelper::RemoveAt
			inline int32 FScriptMapHelperFindPairIndexOffset = 0x0;   // FScriptMapHelper::FindMapPairIndexFromHash
			// Reflected-TSet edit (sibling of the map helpers). RemoveElement = one-call remove-by-value
			// (builds hash/eq itself); RemoveAt = remove by sparse index; AddElement = one-call insert-by-value
			// (builds its own hash/eq/construct/destruct closures, then calls core TScriptSet::Add). All three
			// are out-of-line on builds that don't inline the helper (e.g. DRG/FSD UE4.27 keeps AddElement
			// out-of-line); they stay 0 where the build inlines them (e.g. RC/RogueCore UE5 inlines AddElement,
			// leaving only the variadic core TScriptSet::Add) → that part of set-edit falls to the abort tier.
			inline int32 FScriptSetHelperRemoveElementOffset = 0x0;   // FScriptSetHelper::RemoveElement
			inline int32 FScriptSetHelperRemoveAtOffset = 0x0;        // FScriptSetHelper::RemoveAt
			inline int32 FScriptSetHelperAddElementOffset = 0x0;      // FScriptSetHelper::AddElement
			void InitScriptContainers();
		}

		namespace WeakObject
		{
			// FUObjectArray::AllocateSerialNumber(this=GUObjectArray, int32 Index) -> int32 serial. Lets the SDK
			// build a TWeakObjectPtr for any UObject (ObjectIndex = UObject->InternalIndex; ObjectSerialNumber =
			// AllocateSerialNumber(...)). Engine-stable index math (FUObjectItem 0x18 stride). Module-relative.
			inline int32 AllocateSerialNumberOffset = 0x0;

			/* Distance from the OUTER FUObjectArray to its ObjObjects member.

			   Needed because the two are not the same address and AllocateSerialNumber wants the outer one.
			   GObjects is located by scanning for an address whose CONTENTS match a UObject-array layout, and
			   the thing that matches is ObjObjects — so Off::InSDK::ObjArray::GObjects, and every GObjects in
			   the emitted SDK, is the INNER array. Passing that as AllocateSerialNumber's `this` makes it read
			   NumElements off the wrong field and dereference null+0x10.

			   Engine-stable, hence a constant rather than a scan: FUObjectArray opens with ObjFirstGCIndex,
			   ObjLastNonGCIndex, MaxObjectsNotConsideredByGC (3x int32) and OpenForDisregardForGC (bool +
			   3 pad) before ObjObjects — 0x10 on UE4.27 through UE5.x. Re-check if a game ships a modified
			   FUObjectArray; a wrong value here is a raw call with a bogus `this`, not merely a bad read. */
			inline constexpr int32 FUObjectArrayObjObjectsOffset = 0x10;

			void InitAllocateSerialNumber();
		}

		namespace TypeHash
		{
			// Engine hash functions the emitted SDK GetTypeHash calls at runtime. The int/ptr/FName/float
			// mixes are constexpr in the SDK (no offset needed); only these RVA-resolved functions are found
			// here. All module-relative; 0 if not found (that path then falls back to a loud abort tier).
			//   Strihash   : FCrc::Strihash_DEPRECATED(int Len, const wchar_t*) — FString's case-insensitive
			//                GetTypeHash on BOTH engines (DRG 0x859440 / RC 0x11B1420). Calling it avoids
			//                needing the CRCTable_DEPRECATED address (the function reads its own table).
			//   MemCrc32   : FCrc::MemCrc32(data, len, crc) — UE4.27 default struct memhash (DRG).
			//   CityHash64 : UE5 default struct memhash (RC; low 32 bits used). DRG also exports it.
			inline int32 StrihashOffset = 0x0;
			inline int32 MemCrc32Offset = 0x0;
			inline int32 CityHash64Offset = 0x0;
			void InitTypeHashFunctions();
		}

		namespace ULevel
		{
			inline int32 Actors;
		}

		namespace UDataTable
		{
			inline int32 RowMap;
		}
	}

	namespace FUObjectArray
	{
		inline FFixedUObjectArrayLayout FixedLayout;
		inline FChunkedFixedUObjectArrayLayout ChunkedFixedLayout;

		inline bool bIsChunked = false;

		inline int32 GetObjectsOffset() { return  bIsChunked ? ChunkedFixedLayout.ObjectsOffset : FixedLayout.ObjectsOffset; }
		inline int32 GetNumElementsOffset() { return  bIsChunked ? ChunkedFixedLayout.NumElementsOffset : FixedLayout.NumObjectsOffset; }
		inline int32 GetMaxElementsOffset() { return  bIsChunked ? ChunkedFixedLayout.MaxElementsOffset : FixedLayout.MaxObjectsOffset; }
		inline int32 GetNumChunksOffset() { return  bIsChunked ? ChunkedFixedLayout.NumChunksOffset : 0x0; }
		inline int32 GetMaxChunksOffset() { return  bIsChunked ? ChunkedFixedLayout.MaxChunksOffset : 0x0; }
	}

	namespace FUObjectItem
	{
		inline int32 Flags = -1; // EInternalObjectFlags, behind the object pointer; -1 until confirmed
	}

	namespace NameArray
	{
		inline int32 ChunksStart;
		inline int32 MaxChunkIndex;
		inline int32 NumElements;
		inline int32 ByteCursor;
	}

	/* FField and FFieldClass are all found by Off::Init() on an FProperty game; the values here are the
	   4.25 layout and are never read before that, nor at all on a UProperty game. */
	namespace FField
	{
		inline int32 Vft = 0x00;
		inline int32 Class = 0x08;
		inline int32 Owner = 0x10;
		inline int32 Next = 0x20;
		inline int32 Name = 0x28;
		inline int32 Flags = 0x30;

		inline int32 EditorOnlyMetadata = -1; // Only present in editor builds
	}

	namespace FFieldClass
	{
		inline int32 Name = 0x00;
		inline int32 Id = 0x08;
		inline int32 CastFlags = 0x10; // 0x18 on UE5.7
		inline int32 ClassFlags = 0x18;
		inline int32 SuperClass = 0x20;
	}

	namespace FName
	{
		// Set by OffsetFinder::InitFNameSettings() and checked by PostInitFNameSettings()
		inline int32 CompIdx = 0x0;
		inline int32 Number = 0x4;
	}

	namespace FNameEntry
	{
		// These values are initialized by FNameEntry::Init()
		namespace NameArray
		{
			inline int32 StringOffset;
			inline int32 IndexOffset;
		}

		// These values are initialized by FNameEntry::Init()
		namespace NamePool
		{
			inline int32 HeaderOffset;
			inline int32 StringOffset;
		}
	}

	namespace UObject
	{
		inline int32 Vft;
		inline int32 Flags;
		inline int32 Index;
		inline int32 Class;
		inline int32 Name;
		inline int32 Outer;
	}

	namespace UField
	{
		inline int32 Next;
	}
	namespace UEnum
	{
		inline int32 Names;
		inline int32 UnderlyingType = -1;
	}

	namespace UStruct
	{
		/* Optional offset, if available we can generate a faster IsA implementation for the SDK. */
		inline int32 StructBaseChain = -1;

		inline int32 SuperStruct;
		inline int32 Children;
		inline int32 ChildProperties;
		inline int32 Size;
		inline int32 MinAlignment;
		inline int32 Script;                        // TArray<uint8> bytecode (BP-VM disassembler); -1 unless scripted functions confirm it
		inline int32 StructBaseChainArray;          // FStructBaseChain** for O(1) IsChildOf; OffsetNotFound on pre-UE4.22
		inline int32 NumStructBasesInChainMinusOne; // int32 sibling of StructBaseChainArray; only set when the chain was found
	}

	namespace UFunction
	{
		inline int32 FunctionFlags;
		inline int32 ExecFunction;
	}

	namespace UClass
	{
		inline int32 CastFlags;
		inline int32 ClassDefaultObject;
		inline int32 ImplementedInterfaces;
	}

	namespace FInstancedStruct
	{
		inline int32 ScriptStruct;
		inline int32 StructMemory;
	}

	namespace Property
	{
		inline int32 ArrayDim;
		inline int32 ElementSize;
		inline int32 PropertyFlags;
		inline int32 RepIndex;
		inline int32 BlueprintReplicationCondition;
		inline int32 Offset_Internal;
		inline int32 RepNotifyFunc;
		inline int32 PropertyLinkNext;
		inline int32 NextRef;
		inline int32 DestructorLinkNext;
		inline int32 PostConstructLinkNext;
	}

	namespace ByteProperty
	{
		inline int32 Enum;
	}

	namespace BoolProperty
	{
		struct UBoolPropertyBase
		{
			uint8 FieldSize;
			uint8 ByteOffset;
			uint8 ByteMask;
			uint8 FieldMask;
		};

		inline int32 Base;
	}

	namespace ObjectProperty
	{
		inline int32 PropertyClass;
	}

	namespace InterfaceProperty {
		inline int32 InterfaceClass;
	}

	namespace MulticastDelegateProperty
	{
		inline int32 SignatureFunction;
	}

	namespace ClassProperty
	{
		inline int32 MetaClass;
	}

	namespace StructProperty
	{
		inline int32 Struct;
	}

	namespace ArrayProperty
	{
		inline int32 Inner;
	}

	namespace DelegateProperty
	{
		inline int32 SignatureFunction;
	}

	namespace MapProperty
	{
		struct UMapPropertyBase
		{
			void* KeyProperty;
			void* ValueProperty;
		};

		inline int32 Base;
	}

	namespace SetProperty
	{
		inline int32 ElementProp;
	}

	namespace EnumProperty
	{
		struct UEnumPropertyBase
		{
			void* UnderlayingProperty;
			class UEnum* Enum;
		};

		inline int32 Base;
	}

	namespace FieldPathProperty
	{
		inline int32 FieldClass;
	}

	namespace OptionalProperty
	{
		inline int32 ValueProperty;
	}
}

namespace PropertySizes
{
	/* Measures each size below on a live property of its kind, and logs it. */
	void Init();

	/* These are properties for which their size might change depending on the UE version or compilerflags. */
	inline int32 DelegateProperty = 0x10;
	inline int32 FieldPathProperty = 0x20;
	inline int32 MulticastInlineDelegateProperty = 0x10;
}