#pragma once

#include "Unreal/Enums.h"
#include "../Settings.h"

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
	void Init();

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
			void InitAllocateSerialNumber();
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

	namespace NameArray
	{
		inline int32 ChunksStart;
		inline int32 MaxChunkIndex;
		inline int32 NumElements;
		inline int32 ByteCursor;
	}

	namespace FField
	{
		// Fixed for CasePreserving FNames by OffsetFinder::FixupHardcodedOffsets();
		inline int32 Vft = 0x00;
		inline int32 Class = 0x08;
		inline int32 Owner = 0x10;
		inline int32 Next = 0x20;
		inline int32 Name = 0x28;
		inline int32 Flags = 0x30;
	}

	namespace FFieldClass
	{
		// Fixed for CasePreserving FNames by OffsetFinder::FixupHardcodedOffsets();
		// Fixed for OutlineNumber FNames by OffsetFinder::FixFNameSize();
		inline int32 Name = 0x00;
		inline int32 Id = 0x08;
		inline int32 CastFlags = 0x10; // 0x18 on UE5.7
		inline int32 ClassFlags = 0x18;
		inline int32 SuperClass = 0x20;
	}

	namespace FName
	{
		// These values initialized by OffsetFinder::InitUObjectOffsets()
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
	}

	namespace UStruct
	{
		inline int32 SuperStruct;
		inline int32 Children;
		inline int32 ChildProperties;
		inline int32 Size;
		inline int32 MinAlignment;
		inline int32 Script;                        // TArray<uint8> bytecode (BP-VM disassembler); MinAlignment+4 if no scripted fn to verify
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
	void Init();

	/* These are properties for which their size might change depending on the UE version or compilerflags. */
	inline int32 DelegateProperty = 0x10;
	void InitTDelegateSize();

	inline int32 FieldPathProperty = 0x20;
	void InitFFieldPathSize();

	inline int32 MulticastInlineDelegateProperty = 0x10;
	void InitTMulticastInlineDelegateSize();
}