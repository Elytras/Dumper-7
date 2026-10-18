#pragma once

#include <vector>

#include "Unreal/ObjectArray.h"

#include "Platform.h"

namespace OffsetFinder
{
	constexpr int32 OffsetNotFound = -1;
	constexpr int32 OffsetFinderMinValue = Platform::Is32Bit() ? 0x18 : 0x28;
	constexpr int32 OffsetFinderMaxValue = 0x1A0;

	template<int Alignement = 4, typename T>
	inline int32_t FindOffset(const std::vector<std::pair<void*, T>>& ObjectValuePair, int MinOffset = OffsetFinderMinValue, int MaxOffset = OffsetFinderMaxValue)
	{
		int32_t HighestFoundOffset = MinOffset;
		bool bFoundOffset = false;

		for (int i = 0; i < ObjectValuePair.size(); i++)
		{
			if (ObjectValuePair[i].first == nullptr)
			{
				std::cerr << "Dumper-7 ERROR: FindOffset is skipping ObjectValuePair[" << i << "] because .first is nullptr." << std::endl;
				continue;
			}

			for (int j = HighestFoundOffset; j < MaxOffset; j += Alignement)
			{
				const T TypedValueAtOffset = *reinterpret_cast<T*>(static_cast<uint8_t*>(ObjectValuePair[i].first) + j);

				if (TypedValueAtOffset == ObjectValuePair[i].second && j >= HighestFoundOffset)
				{
					bFoundOffset = true;

					if (j > HighestFoundOffset)
					{
						HighestFoundOffset = j;
						i = 0;
					}
					j = MaxOffset;
				}
			}
		}

		//return HighestFoundOffset != MinOffset ? HighestFoundOffset : OffsetNotFound;
		return bFoundOffset ? HighestFoundOffset : OffsetNotFound;
	}

	template<bool bCheckForVft = true>
	inline int32_t GetValidPointerOffset(const void* PtrObjA, const void* PtrObjB, int32_t StartingOffset, int32_t MaxOffset, bool bNeedsToBeInProcessMemory = false)
	{
		const uint8_t* ObjA = static_cast<const uint8_t*>(PtrObjA);
		const uint8_t* ObjB = static_cast<const uint8_t*>(PtrObjB);

		if (Platform::IsBadReadPtr(ObjA) || Platform::IsBadReadPtr(ObjB))
			return OffsetNotFound;

		for (int j = StartingOffset; j <= MaxOffset; j += sizeof(void*))
		{
			const bool bIsAValid = !Platform::IsBadReadPtr(*reinterpret_cast<void* const*>(ObjA + j)) && (bCheckForVft ? !Platform::IsBadReadPtr(**reinterpret_cast<void** const*>(ObjA + j)) : true);
			const bool bIsBValid = !Platform::IsBadReadPtr(*reinterpret_cast<void* const*>(ObjB + j)) && (bCheckForVft ? !Platform::IsBadReadPtr(**reinterpret_cast<void** const*>(ObjB + j)) : true);

			if (bNeedsToBeInProcessMemory)
			{
				if (!Platform::IsAddressInProcessRange(*reinterpret_cast<void* const*>(ObjA + j)) || !Platform::IsAddressInProcessRange(*reinterpret_cast<void* const*>(ObjB + j)))
					continue;
			}

			if (bIsAValid && bIsBValid)
				return j;
		}

		return OffsetNotFound;
	};

	/* Whether Ptr is a live UObject: readable, and its own InternalIndex leads back to it in GObjects.
	   The finders use it to accept a pointer they found only after this round trip, so a candidate offset
	   that holds some other number is rejected instead of dereferenced as an object. */
	bool IsLiveObject(const void* Ptr);

	/* Whether Ptr is a property (FField, or UProperty before 4.25) whose owner is Owner - what an
	   ArrayProperty's Inner, a MapProperty's key and an EnumProperty's underlying property all are. */
	bool IsPropertyOwnedBy(const void* Ptr, const void* Owner);

	/* Up to Max properties of one kind, from the structs in GObjects in index order: what the finders for a
	   property kind's own members check their candidate slot against, and how Off::Init() tells a kind the
	   game never uses from one whose layout wasn't found. */
	std::vector<UEProperty> SampleProperties(EClassCastFlags Type, size_t Max);

	/* UObject */
	int32_t FindUObjectFlagsOffset();
	int32_t FindUObjectIndexOffset();
	int32_t FindUObjectClassOffset();
	int32_t FindUObjectNameOffset();
	int32_t FindUObjectOuterOffset();

	void FixupHardcodedOffsets();
	void InitFNameSettings();
	bool PostInitFNameSettings();

	/* UField */
	int32_t FindUFieldNextOffset();

	/* FField */
	int32_t FindFFieldOwnerOffset();
	int32_t FindFFieldNextOffset();
	int32_t FindFFieldNameOffset();
	int32_t NewFindFFieldNameOffset();
	int32_t FindFFieldClassOffset();
	int32_t FindFFieldFlagsOffset();
	int32_t FindFFieldEditorOnlyMetaDataOffset();

	/* FFieldClass */
	int32_t FindFieldClassCastFlagsOffset();
	int32_t FindFieldClassIdOffset();
	int32_t FindFieldClassSuperClassOffset();

	/* UEnum */
	int32_t FindEnumNamesOffset();
	int32_t FindEnumUnderlayingTypeOffset();

	/* UStruct */
	int32_t FindSuperOffset();
	int32_t FindChildOffset();
	int32_t FindChildPropertiesOffset();
	int32_t FindStructSizeOffset();
	int32_t FindMinAlignmentOffset();
	int32_t FindScriptOffset();
	int32_t FindStructBaseChainOffset();

	/* UFunction */
	int32_t FindFunctionFlagsOffset();
	int32_t FindFunctionNativeFuncOffset();

	/* UClass */
	int32_t FindCastFlagsOffset();
	int32_t FindDefaultObjectOffset();
	int32_t FindImplementedInterfacesOffset();

	/* Property */
	int32_t FindElementSizeOffset();
	int32_t FindArrayDimOffset();
	int32_t FindPropertyFlagsOffset();
	int32_t FindOffsetInternalOffset();
	int32 FindRepIndexOffset(int32 PropertyFlagsOffset, int32 OffsetInternalOffset);
	int32 FindBlueprintReplicationConditionOffset(int32 RepIndexOffset);
	int32 FindRepNotifyFuncOffset(int32 OffsetInternalOffset, int32 PropertySize);
	int32 FindPropertyLinkNextOffset(int32 OffsetInternalOffset);
	int32 FindPropertyChainOffset(int32 PropertyLinkNextOffset, int32 ChainIndex);

	/* FieldPathProperty */
	int32 FindFieldPathPropertyFieldClassOffset(int32 PropertySize);

	/* OptionalProperty */
	int32 FindOptionalPropertyValuePropertyOffset(int32 PropertySize);

	/* BoolProperty */
	int32_t FindBoolPropertyBaseOffset();

	/* ObjectProperty */
	int32_t FindObjectPropertyClassOffset(int32 PropertySize);

	/* ClassProperty */
	int32_t FindClassPropertyMetaClassOffset(int32 PropertyClassOffset);

	/* InterfaceProperty */
	int32_t FindInterfacePropertyInterfaceClassOffset(int32 PropertySize);

	/* EnumProperty */
	int32_t FindEnumPropertyBaseOffset();

	/* ByteProperty */
	int32_t FindBytePropertyEnumOffset();

	/* StructProperty */
	int32_t FindStructPropertyStructOffset(int32 PropertySize);

	/* DelegateProperty */
	int32_t FindDelegatePropertySignatureFunctionOffset(int32 PropertySize);

	/* MulticastDelegateProperty */
	int32_t FindMulticastDelegatePropertySignatureFunctionOffset(int32 PropertySize);

	/* ArrayProperty */
	int32_t FindInnerTypeOffset(const int32 PropertySize);

	/* SetProperty */
	int32_t FindSetPropertyBaseOffset(const int32 PropertySize);

	/* MapProperty */
	int32_t FindMapPropertyBaseOffset(const int32 PropertySize);

	/* InSDK -> ULevel */
	int32_t FindLevelActorsOffset();

	/* InSDK -> UDataTable */
	int32_t FindDatatableRowMapOffset();

	/* FUObjectItem */
	int32 FindFUObjectItemFlagsOffset();
}