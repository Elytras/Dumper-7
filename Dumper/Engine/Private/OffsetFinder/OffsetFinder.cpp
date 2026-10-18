#include <algorithm>
#include <vector>
#include <random>

#include "OffsetFinder/OffsetFinder.h"
#include "Unreal/ObjectArray.h"

#include "Platform.h"

bool OffsetFinder::IsLiveObject(const void* Ptr)
{
	if (!Ptr || (reinterpret_cast<uintptr_t>(Ptr) & (alignof(void*) - 1)) != 0)
		return false;

	const uint8* const IndexSlot = static_cast<const uint8*>(Ptr) + Off::UObject::Index;
	if (Platform::IsBadReadPtr(Ptr) || Platform::IsBadReadPtr(IndexSlot))
		return false;

	const int32 Index = *reinterpret_cast<const int32*>(IndexSlot);

	return Index >= 0 && Index < ObjectArray::Num() && ObjectArray::GetByIndex(Index).GetAddress() == Ptr;
}

bool OffsetFinder::IsPropertyOwnedBy(const void* Ptr, const void* Owner)
{
	if (!Ptr || (reinterpret_cast<uintptr_t>(Ptr) & (alignof(void*) - 1)) != 0)
		return false;

	/* Before 4.25 a property is a UObject, and its owner is its Outer. */
	if (!Settings::Internal::bUseFProperty)
		return IsLiveObject(Ptr) && UEObject(const_cast<void*>(Ptr)).GetOuter().GetAddress() == Owner;

	const uint8* const OwnerSlot = static_cast<const uint8*>(Ptr) + Off::FField::Owner;
	if (Platform::IsBadReadPtr(Ptr) || Platform::IsBadReadPtr(OwnerSlot))
		return false;

	/* Since 5.1.1 FFieldVariant tags a UObject owner in the pointer's low bit instead of a bool. */
	uintptr_t OwnerValue = *reinterpret_cast<const uintptr_t*>(OwnerSlot);
	if (Settings::Internal::bUseMaskForFieldOwner)
		OwnerValue &= ~static_cast<uintptr_t>(0x1);

	return OwnerValue == reinterpret_cast<uintptr_t>(Owner);
}

std::vector<UEProperty> OffsetFinder::SampleProperties(EClassCastFlags Type, size_t Max)
{
	std::vector<UEProperty> Samples;

	for (UEObject Obj : ObjectArray())
	{
		if (!Obj.IsA(EClassCastFlags::Struct))
			continue;

		for (UEProperty Prop : Obj.Cast<UEStruct>().GetProperties())
		{
			if (!Prop.IsA(Type))
				continue;

			Samples.push_back(Prop);

			if (Samples.size() == Max)
				return Samples;
		}
	}

	return Samples;
}

static const void* ReadPtr(const UEProperty& Prop, int32 Offset)
{
	return *reinterpret_cast<const void* const*>(static_cast<const uint8*>(Prop.GetAddress()) + Offset);
}

/* Whether every sample holds a live object of type ObjectType at Offset: how a property kind's own
   UObject member (signature function, interface class, struct, ...) is told apart from whatever
   happens to sit next to it. */
static bool HoldObjectsOfType(const std::vector<UEProperty>& Samples, int32 Offset, EClassCastFlags ObjectType)
{
	if (Samples.empty() || Offset == OffsetFinder::OffsetNotFound)
		return false;

	return std::all_of(Samples.begin(), Samples.end(), [&](const UEProperty& Prop)
	{
		const void* Value = ReadPtr(Prop, Offset);
		return OffsetFinder::IsLiveObject(Value) && UEObject(const_cast<void*>(Value)).IsA(ObjectType);
	});
}

/* Where a property of kind Type keeps the NumSlots properties it wraps (an ArrayProperty's Inner, a
   MapProperty's key and value, ...): the first slot from PropertySize on that holds, in every sample,
   properties owned by the sample itself. Some games add a member in front of them, hence the second
   candidate. */
static int32 FindOwnedPropertySlot(EClassCastFlags Type, int32 PropertySize, int32 NumSlots)
{
	if (PropertySize == OffsetFinder::OffsetNotFound)
		return OffsetFinder::OffsetNotFound;

	const std::vector<UEProperty> Samples = OffsetFinder::SampleProperties(Type, 8);

	for (int32 Offset = PropertySize; !Samples.empty() && Offset <= PropertySize + static_cast<int32>(sizeof(void*)); Offset += sizeof(void*))
	{
		const bool bAllOwned = std::all_of(Samples.begin(), Samples.end(), [&](const UEProperty& Prop)
		{
			for (int32 Slot = 0; Slot < NumSlots; Slot++)
			{
				if (!OffsetFinder::IsPropertyOwnedBy(ReadPtr(Prop, Offset + Slot * sizeof(void*)), Prop.GetAddress()))
					return false;
			}

			return true;
		});

		if (bAllOwned)
			return Offset;
	}

	return OffsetFinder::OffsetNotFound;
}

/* UObject */
int32_t OffsetFinder::FindUObjectFlagsOffset()
{
	constexpr auto EnumFlagValueToSearch = 0x43;

	/* We're looking for a commonly occuring flag and this number basically defines the minimum number that counts ad "commonly occuring". */
	constexpr auto MinNumFlagValuesRequiredAtOffset = 0xA0;

	for (int i = 0; i < 0x20; i++)
	{
		int Offset = 0x0;
		while (Offset != OffsetNotFound)
		{
			// Look for 0x43 in this object, as it is a really common value for UObject::Flags
			Offset = FindOffset(std::vector{ std::pair{ ObjectArray::GetByIndex(i).GetAddress(), EnumFlagValueToSearch } }, Offset, 0x40);

			if (Offset == OffsetNotFound)
				break; // Early exit

			/* We're looking for a common flag. To check if the flag  is common we're checking the first 0x100 objects to see how often the flag occures at this offset. */
			int32 NumObjectsWithFlagAtOffset = 0x0;

			int Counter = 0;
			for (UEObject Obj : ObjectArray())
			{
				// Only check the (possible) flags of the first 0x100 objects
				if (Counter++ == 0x100)
					break;

				const int32 TypedValueAtOffset = *reinterpret_cast<int32*>(reinterpret_cast<uintptr_t>(Obj.GetAddress()) + Offset);

				if (TypedValueAtOffset == EnumFlagValueToSearch)
					NumObjectsWithFlagAtOffset++;
			}

			if (NumObjectsWithFlagAtOffset > MinNumFlagValuesRequiredAtOffset)
				return Offset;
		}
	}

	return OffsetNotFound;
}

int32_t OffsetFinder::FindUObjectIndexOffset()
{
	std::vector<std::pair<void*, int32_t>> Infos;

	Infos.emplace_back(ObjectArray::GetByIndex(0x055).GetAddress(), 0x055);
	Infos.emplace_back(ObjectArray::GetByIndex(0x123).GetAddress(), 0x123);

	return FindOffset<4>(Infos, sizeof(void*)); // Skip VTable
}

int32_t OffsetFinder::FindUObjectClassOffset()
{
	/* Checks for a pointer that points to itself in the end. The UObject::Class pointer of "Class CoreUObject.Class" will point to "Class CoreUObject.Class". */
	auto IsValidCyclicUClassPtrOffset = [](const uint8_t* ObjA, const uint8_t* ObjB, int32_t ClassPtrOffset)
		{
			/* Will be advanced before they are used. */
			const uint8_t* NextClassA = ObjA;
			const uint8_t* NextClassB = ObjB;

			for (int MaxLoopCount = 0; MaxLoopCount < 0x10; MaxLoopCount++)
			{
				const uint8_t* CurrentClassA = NextClassA;
				const uint8_t* CurrentClassB = NextClassB;

				NextClassA = *reinterpret_cast<const uint8_t* const*>(NextClassA + ClassPtrOffset);
				NextClassB = *reinterpret_cast<const uint8_t* const*>(NextClassB + ClassPtrOffset);

				/* If this was UObject::Class it would never be invalid. The pointer would simply point to itself.*/
				if (!NextClassA || !NextClassB || Platform::IsBadReadPtr(NextClassA) || Platform::IsBadReadPtr(NextClassB))
					return false;

				if (CurrentClassA == NextClassA && CurrentClassB == NextClassB)
					return true;
			}

			return false;
		};

	const uint8_t* const ObjA = static_cast<const uint8_t*>(ObjectArray::GetByIndex(0x055).GetAddress());
	const uint8_t* const ObjB = static_cast<const uint8_t*>(ObjectArray::GetByIndex(0x123).GetAddress());

	int32_t Offset = 0;
	while (Offset != OffsetNotFound)
	{
		Offset = GetValidPointerOffset<true>(ObjA, ObjB, Offset + sizeof(void*), 0x50);

		if (IsValidCyclicUClassPtrOffset(ObjA, ObjB, Offset))
			return Offset;
	}

	return OffsetNotFound;
}

/*
* IsPotentialValidOffset: A function to filter offsets that can not possibly be valid for UObject::Name or FField::Name.
*						  Example for UObject::Name: it can 100% not be at the same offset as UObject::Class
*
* DataGatherer: A function to gather values at the offsets not filterd by 'IsPotentialValidOffset'. Data is later used to filter more offsets, until hopefully only one is left.
*/
template<typename IteratorType>
int32_t FindNameOffsetForSomeClass(std::function<bool(int32_t Value)> IsPotentialValidOffset, IteratorType DataSetStartIterator, IteratorType DataSetEndIterator)
{
	/*
	* Requirements:
	*	- CmpIdx > 0x10 && CmpIdx < 0xF0000000
	*	- AverageValue >= 0x100 && AverageValue <= 0xFF00000;
	*	- Offset != { OtherOffsets }
	*/

	/* A struct describing the value */
	struct ValueInfo
	{
		int32 Offset;					   // Offset from the UObject start to this value
		int32 NumNamesWithLowCmpIdx = 0x0; // The number of names where the comparison index is in the range [0, 16]. Usually this should be far less than 0x20 names.
		uint64 TotalValue = 0x0;		   // The total value of the int32 data at this offset over all objects in GObjects
		bool bIsValidCmpIdxRange = true;   // Whether this value could be a valid FName::ComparisonIndex
	};


	std::vector<ValueInfo> PossibleOffsets;

	constexpr auto MaxAllowedComparisonIndexValue = 0x4000000; // Somewhat arbitrary limit. Make sure this isn't too low for games on FNamePool with lots of names and 0x14 block-size bits

	constexpr auto MaxAllowedAverageComparisonIndexValue = MaxAllowedComparisonIndexValue / 2; // Also somewhat arbitrary limit, but the average value shouldn't be as high as the max allowed one
	constexpr auto MinAllowedAverageComparisonIndexValue = 0x280; // If the average name is below 0x100 it is either the smallest UE application ever, or not the right offset

	constexpr auto LowComparisonIndexUpperCap = 0x10; // The upper limit of what is considered a "low" comparison index
	constexpr auto MaxAllowedNamesWithLowCmpIdx = 0x40;


	for (int i = sizeof(void*); i <= 0x40; i += 0x4)
	{
		if (!IsPotentialValidOffset(i))
			continue;

		PossibleOffsets.push_back(ValueInfo{ i });
	}

	auto GetDataAtOffsetAsInt = [](const void* Ptr, int32 Offset) -> uint32 { return *reinterpret_cast<const uint32*>(reinterpret_cast<const uintptr_t>(Ptr) + Offset); };

	int NumObjectsConsidered = 0;

	for (; DataSetStartIterator != DataSetEndIterator; ++DataSetStartIterator)
	{
		constexpr auto X86SmallPageSize = 0x1000;
		constexpr auto MaxAccessedSizeInUObject = 0x44;

		const void* CurrentObjectOrField = (*DataSetStartIterator).GetAddress();

		/*
		* Purpose: Make sure all offsets in the UObject::Name finder can be accessed
		* Reasoning: Objects are allocated in Blocks, these allocations are page-aligned in both size and base. If an object + MaxAccessedSizeInUObject goes past the page-bounds
		*            it might also go past the extends of an allocation. There's no reliable way of getting the size of UObject without knowing it's offsets first.
		*/
		const bool bIsGoingPastPageBounds = (reinterpret_cast<const uintptr_t>(CurrentObjectOrField) & (X86SmallPageSize - 1)) > (X86SmallPageSize - MaxAccessedSizeInUObject);
		if (bIsGoingPastPageBounds)
			continue;

		NumObjectsConsidered++;

		for (ValueInfo& Info : PossibleOffsets)
		{
			const uint32 ValueAtOffset = GetDataAtOffsetAsInt(CurrentObjectOrField, Info.Offset);

			Info.TotalValue += ValueAtOffset;
			Info.bIsValidCmpIdxRange = Info.bIsValidCmpIdxRange && ValueAtOffset < MaxAllowedComparisonIndexValue;
			Info.NumNamesWithLowCmpIdx += (ValueAtOffset <= LowComparisonIndexUpperCap);
		}
	}

	int32 FirstValidOffset = -1;
	for (const ValueInfo& Info : PossibleOffsets)
	{
		const auto AverageValue = (Info.TotalValue / NumObjectsConsidered);

		if (Info.bIsValidCmpIdxRange && Info.NumNamesWithLowCmpIdx <= MaxAllowedNamesWithLowCmpIdx
			&& AverageValue >= MinAllowedAverageComparisonIndexValue && AverageValue <= MaxAllowedAverageComparisonIndexValue)
		{
			if (FirstValidOffset == -1)
			{
				FirstValidOffset = Info.Offset;
				continue;
			}

			/* This shouldn't be the case, so log it as an info but continue, as the first offset is still likely the right one. */
			std::cerr << std::format("Dumper-7: Another [UObject/FField]::Name offset (0x{:04X}) is also considered valid.\n", Info.Offset);
		}
	}

	return FirstValidOffset;
}

int32_t OffsetFinder::FindUObjectNameOffset()
{
	auto IsPotentiallyValidOffset = [](int32 Offset) -> bool
		{
			// Make sure 0x4 aligned Offsets are neither the start, nor the middle of a pointer-member. Irrelevant for 32-bit, because the 2nd check will be 0x2 aligned then.
			return Offset != Off::UObject::Class && Offset != (Off::UObject::Class + (sizeof(void*) / 2))
				&& Offset != Off::UObject::Outer && Offset != (Off::UObject::Outer + (sizeof(void*) / 2))
				&& Offset != Off::UObject::Flags
				&& Offset != Off::UObject::Index
				&& Offset != Off::UObject::Vft && Offset != (Off::UObject::Vft + (sizeof(void*) / 2));
		};

	return FindNameOffsetForSomeClass(IsPotentiallyValidOffset, ObjectArray().begin(), ObjectArray().end());
}

int32_t OffsetFinder::FindUObjectOuterOffset()
{
	int32_t LowestFoundOffset = 0xFFFF;

	const int32 NumObjects = ObjectArray::Num();
	if (NumObjects < 2)
		return OffsetNotFound;

	/* Seed with the address of the first object so it varies per game but is deterministic per run. */
	std::mt19937 Rng(static_cast<uint32_t>(reinterpret_cast<uintptr_t>(ObjectArray::GetByIndex(0).GetAddress())));
	const int32_t UpperBound = (NumObjects - 1 < 0x3FF) ? (NumObjects - 1) : 0x3FF;
	std::uniform_int_distribution<int32_t> Dist(0, UpperBound);

	// loop a few times in case we accidentally choose a UPackage (which doesn't have an Outer) to find Outer
	for (int i = 0; i < 0x10; i++)
	{
		int32_t Offset = 0;

		const int32_t IndexA = Dist(Rng);
		int32_t IndexB = Dist(Rng);
		if (IndexB == IndexA)
			IndexB = (IndexA + 1) % (UpperBound + 1);

		const void* ObjA = ObjectArray::GetByIndex(IndexA).GetAddress();
		const void* ObjB = ObjectArray::GetByIndex(IndexB).GetAddress();

		while (Offset != OffsetNotFound)
		{
			Offset = GetValidPointerOffset(ObjA, ObjB, Offset + sizeof(void*), 0x50);

			// Make sure we didn't re-find the Class offset or Index (if the Index filed is a valid pionter for some ungodly reason). 
			if (Offset != Off::UObject::Class && Offset != Off::UObject::Index)
				break;
		}

		if (Offset != OffsetNotFound && Offset < LowestFoundOffset)
			LowestFoundOffset = Offset;
	}

	return LowestFoundOffset == 0xFFFF ? OffsetNotFound : LowestFoundOffset;
}

/*
FFieldVariant, the FField::Owner member, is { void*, bool } (padded to 0x10) before 5.1.1 and a lone
tagged pointer since. This decides which by looking at the slot right after the owner pointer of the
first property of a few structs: with the bool it holds 0 or 1 and padding, without it it is FField::Next.
Every struct named here declares more than one property, so that Next is never null and can't pass for
a bool. Runs before FField::Next is known (its finder needs the answer), hence the first child only, and
sets Settings::Internal::bUseMaskForFieldOwner, which IsPropertyOwnedBy also relies on.
*/
void OffsetFinder::FixupHardcodedOffsets()
{
	if (!Settings::Internal::bUseFProperty || Off::FField::Owner == OffsetNotFound)
		return;

	int32 NumChecked = 0;
	int32 NumPointers = 0;

	for (const char* StructName : { "Actor", "ActorComponent", "Pawn", "Guid", "Vector" })
	{
		const UEStruct Struct = ObjectArray::FindStructFast(StructName);
		if (!Struct || !Struct.GetChildProperties())
			continue;

		const void* const SlotAfterOwner = static_cast<const uint8*>(Struct.GetChildProperties().GetAddress()) + Off::FField::Owner + sizeof(void*);
		const void* const PossibleNext = *static_cast<void* const*>(SlotAfterOwner);

		NumChecked++;
		NumPointers += !Platform::IsBadReadPtr(PossibleNext) && (reinterpret_cast<uintptr_t>(PossibleNext) & 0x1) == 0;
	}

	Settings::Internal::bUseMaskForFieldOwner = NumChecked > 0 && NumPointers == NumChecked;
	std::cerr << std::format("FFieldVariant: {} ({} of {} properties hold a pointer after the owner)\n",
		Settings::Internal::bUseMaskForFieldOwner ? "tagged pointer, 5.1.1+" : "pointer + bool", NumPointers, NumChecked);
}

void OffsetFinder::InitFNameSettings()
{
	UEObject FirstObject = ObjectArray::GetByIndex(0);

	const uint8* NameAddress = static_cast<const uint8*>(FirstObject.GetFName().GetAddress());

	const int32 FNameFirstInt /* ComparisonIndex */ = *reinterpret_cast<const int32*>(NameAddress);
	const int32 FNameSecondInt /* [Number/DisplayIndex] */ = *reinterpret_cast<const int32*>(NameAddress + 0x4);

	/* Some games move 'Name' before 'Class'. Just substract the offset of 'Name' with the offset of the member that follows right after it, to get an estimate of sizeof(FName). */
	const int32 FNameSize = !Settings::Internal::bIsObjectNameBeforeClass ? (Off::UObject::Outer - Off::UObject::Name) : (Off::UObject::Class - Off::UObject::Name);

	Off::FName::CompIdx = 0x0;
	Off::FName::Number = 0x4; // defaults for check

	// FNames for which FName::Number == [1...4]
	auto GetNumNamesWithNumberOneToFour = []() -> int32
		{
			int32 NamesWithNumberOneToFour = 0x0;

			for (UEObject Obj : ObjectArray())
			{
				const uint32 Number = Obj.GetFName().GetNumber();

				if (Number > 0x0 && Number < 0x5)
					NamesWithNumberOneToFour++;
			}

			return NamesWithNumberOneToFour;
		};

	/*
	* Games without FNAME_OUTLINE_NUMBER have a min. percentage of 6% of all object-names for which FName::Number is in a [1...4] range
	* On games with FNAME_OUTLINE_NUMBER the (random) integer after FName::ComparisonIndex is in the range from [1...4] about 2% (or less) of times.
	*
	* The minimum percentage of names is set to 3% to give both normal names, as well as outline-numer names a buffer-zone.
	*
	* This doesn't work on some very small UE template games, which is why PostInitFNameSettings() was added to fix the incorrect behavior of this function
	*/
	constexpr float MinPercentage = 0.03f;

	/* Minimum required ammount of names for which FName::Number is in a [1...4] range */
	const int32 FNameNumberThreashold = static_cast<int32>(ObjectArray::Num() * MinPercentage);

	Off::FName::CompIdx = 0x0;

	if (FNameSize == 0x8 && FNameFirstInt == FNameSecondInt) /* WITH_CASE_PRESERVING_NAME + FNAME_OUTLINE_NUMBER */
	{
		Settings::Internal::bUseCasePreservingName = true;
		Settings::Internal::bUseOutlineNumberName = true;

		Off::FName::Number = -0x1;
		Off::InSDK::Name::FNameSize = 0x8;
	}
	else if (FNameSize == 0x10) /* WITH_CASE_PRESERVING_NAME */
	{
		Settings::Internal::bUseCasePreservingName = true;

		Off::FName::Number = FNameFirstInt == FNameSecondInt ? 0x8 : 0x4;

		Off::InSDK::Name::FNameSize = 0xC;
	}
	else if (GetNumNamesWithNumberOneToFour() < FNameNumberThreashold) /* FNAME_OUTLINE_NUMBER */
	{
		Settings::Internal::bUseOutlineNumberName = true;

		Off::FName::Number = -0x1;

		Off::InSDK::Name::FNameSize = 0x4;
	}
	else /* Default */
	{
		Off::FName::Number = 0x4;

		Off::InSDK::Name::FNameSize = 0x8;
	}
}

/* Cross-checks the FName layout InitFNameSettings() guessed against the size of a real NameProperty,
   APlayerStart::PlayerStartTag, and corrects it. Returns false when that property isn't there to check
   against, so the guess stands unverified. */
bool OffsetFinder::PostInitFNameSettings()
{
	const UEClass PlayerStart = ObjectArray::FindClassFast("PlayerStart");
	const UEProperty PlayerStartTag = PlayerStart ? PlayerStart.FindMember("PlayerStartTag", EClassCastFlags::NameProperty) : UEProperty(nullptr);

	if (!PlayerStartTag)
		return false;

	const int32 FNameSize = PlayerStartTag.GetSize();

	/* Nothing to do for us, everything is fine! */
	if (Off::InSDK::Name::FNameSize == FNameSize)
		return true;

	const uint8* NameAddress = static_cast<const uint8*>(PlayerStart.GetFName().GetAddress());

	const int32 FNameFirstInt /* ComparisonIndex */ = *reinterpret_cast<const int32*>(NameAddress);
	const int32 FNameSecondInt /* [Number/DisplayIndex] */ = *reinterpret_cast<const int32*>(NameAddress + 0x4);

	if (FNameSize == 0x8 && FNameFirstInt == FNameSecondInt) /* WITH_CASE_PRESERVING_NAME + FNAME_OUTLINE_NUMBER */
	{
		Settings::Internal::bUseCasePreservingName = true;
		Settings::Internal::bUseOutlineNumberName = true;

		Off::FName::Number = -0x1;
		Off::InSDK::Name::FNameSize = 0x8;
	}
	else if (FNameSize > 0x8) /* WITH_CASE_PRESERVING_NAME */
	{
		Settings::Internal::bUseOutlineNumberName = false;
		Settings::Internal::bUseCasePreservingName = true;

		Off::FName::Number = FNameFirstInt == FNameSecondInt ? 0x8 : 0x4;

		Off::InSDK::Name::FNameSize = 0xC;
	}
	else if (FNameSize == 0x4) /* FNAME_OUTLINE_NUMBER */
	{
		Settings::Internal::bUseOutlineNumberName = true;
		Settings::Internal::bUseCasePreservingName = false;

		Off::FName::Number = -0x1;

		Off::InSDK::Name::FNameSize = 0x4;
	}
	else /* Default */
	{
		Settings::Internal::bUseOutlineNumberName = false;
		Settings::Internal::bUseCasePreservingName = false;

		Off::FName::Number = 0x4;
		Off::InSDK::Name::FNameSize = 0x8;
	}

	return true;
}

/* UField */

/* UField::Next links a class's functions: the first function of KismetSystemLibrary and of
   KismetStringLibrary each point there at another live function of the same class. */
int32_t OffsetFinder::FindUFieldNextOffset()
{
	const UEStruct KismetSystemLibrary = ObjectArray::FindObjectFast<UEStruct>("KismetSystemLibrary");
	const UEStruct KismetStringLibrary = ObjectArray::FindObjectFast<UEStruct>("KismetStringLibrary");

	if (!KismetSystemLibrary || !KismetStringLibrary)
		return OffsetNotFound;

	const UEObject KismetSystemLibraryChild = KismetSystemLibrary.GetChild();
	const UEObject KismetStringLibraryChild = KismetStringLibrary.GetChild();

	if (!KismetSystemLibraryChild || !KismetStringLibraryChild)
		return OffsetNotFound;

#undef max
	const auto HighestUObjectOffset = std::max({ Off::UObject::Index, Off::UObject::Name, Off::UObject::Flags, Off::UObject::Outer, Off::UObject::Class });
#define max(a,b)            (((a) > (b)) ? (a) : (b))

	auto LinksToSibling = [](const UEObject& Child, int32 Offset)
	{
		void* const Next = *reinterpret_cast<void* const*>(static_cast<const uint8*>(Child.GetAddress()) + Offset);
		return IsLiveObject(Next) && UEObject(Next).GetOuter() == Child.GetOuter();
	};

	for (int32 Offset = Align(HighestUObjectOffset + 0x4, static_cast<int>(sizeof(void*))); Offset <= 0x60; Offset += sizeof(void*))
	{
		if (LinksToSibling(KismetSystemLibraryChild, Offset) && LinksToSibling(KismetStringLibraryChild, Offset))
			return Offset;
	}

	return OffsetNotFound;
}

/* FField */

/* FField::Owner holds the object a property belongs to: for a struct's own members, the UScriptStruct
   itself. So it is the slot after FField::ClassPrivate that points back at the struct, in every member
   checked. The low bit is masked because 5.1.1+ tags a UObject owner there; pointers are 8-aligned, so
   the mask changes nothing on older engines. */
int32_t OffsetFinder::FindFFieldOwnerOffset()
{
	std::vector<std::pair<const uint8*, uintptr_t>> FieldsAndOwners;

	for (const char* StructName : { "Guid", "Vector", "Color" })
	{
		const UEStruct Struct = ObjectArray::FindStructFast(StructName);
		if (Struct && Struct.GetChildProperties())
			FieldsAndOwners.emplace_back(static_cast<const uint8*>(Struct.GetChildProperties().GetAddress()), reinterpret_cast<uintptr_t>(Struct.GetAddress()));
	}

	if (FieldsAndOwners.size() < 2)
		return OffsetNotFound;

	for (int32 Offset = Off::FField::Class + sizeof(void*); Offset <= 0x30; Offset += sizeof(void*))
	{
		const bool bAllPointBack = std::all_of(FieldsAndOwners.begin(), FieldsAndOwners.end(), [Offset](const auto& FieldAndOwner)
		{
			const uintptr_t Value = *reinterpret_cast<const uintptr_t*>(FieldAndOwner.first + Offset);
			return (Value & ~static_cast<uintptr_t>(0x1)) == FieldAndOwner.second;
		});

		if (bAllPointBack)
			return Offset;
	}

	return OffsetNotFound;
}

/* FField::Next follows FFieldVariant, which is one tagged pointer since 5.1.1 and a pointer plus a
   padded bool before. The first member of FGuid and of FVector links there to another member of its
   own struct. */
int32_t OffsetFinder::FindFFieldNextOffset()
{
	const UEStruct Guid = ObjectArray::FindStructFast("Guid");
	const UEStruct Vector = ObjectArray::FindStructFast("Vector");

	if (!Guid || !Vector || !Guid.GetChildProperties() || !Vector.GetChildProperties() || Off::FField::Owner == OffsetNotFound)
		return OffsetNotFound;

	const int32 FFieldVariantSize = Settings::Internal::bUseMaskForFieldOwner ? sizeof(void*) : 2 * sizeof(void*);

	auto LinksToSibling = [](const UEStruct& Struct, int32 Offset)
	{
		const uint8* const FirstMember = static_cast<const uint8*>(Struct.GetChildProperties().GetAddress());
		return IsPropertyOwnedBy(*reinterpret_cast<void* const*>(FirstMember + Offset), Struct.GetAddress());
	};

	for (int32 Offset = Off::FField::Owner + FFieldVariantSize; Offset <= 0x48; Offset += sizeof(void*))
	{
		if (LinksToSibling(Guid, Offset) && LinksToSibling(Vector, Offset))
			return Offset;
	}

	return OffsetNotFound;
}

int32_t OffsetFinder::FindFFieldNameOffset()
{
	UEFField GuidChild = ObjectArray::FindStructFast("Guid").GetChildProperties();
	UEFField VectorChild = ObjectArray::FindStructFast("Vector").GetChildProperties();

	/* The name follows Next on every version, so the owner and link pointers before it are never
	   handed to the name lookup as an index. */
	for (Off::FField::Name = Off::FField::Next + sizeof(void*); Off::FField::Name < 0x40; Off::FField::Name += 4)
	{
		const std::string GuidChildName = GuidChild.GetName();
		const std::string VectorChildName = VectorChild.GetName();

		if ((GuidChildName == "A" || GuidChildName == "D") && (VectorChildName == "X" || VectorChildName == "Z"))
			return Off::FField::Name;
	}

	return OffsetNotFound;
}

int32_t OffsetFinder::NewFindFFieldNameOffset()
{
	auto IsPotentiallyValidOffset = [](int32 Offset) -> bool
		{
			// Make sure 0x4 aligned Offsets are neither the start, nor the middle of a pointer-member. Irrelevant for 32-bit, because the 2nd check will be 0x2 aligned then.
			return Offset != Off::FField::Class && Offset != (Off::FField::Class + (sizeof(void*) / 2))
				&& Offset != Off::FField::Next && Offset != (Off::FField::Next + (sizeof(void*) / 2))
				&& Offset != Off::FField::Vft && Offset != (Off::FField::Vft + (sizeof(void*) / 2));
		};

	AllFieldIterator TmpIt;

	return FindNameOffsetForSomeClass(IsPotentiallyValidOffset, TmpIt.begin(), TmpIt.end());
}

int32_t OffsetFinder::FindFFieldEditorOnlyMetaDataOffset()
{
	const UEFField GuidChild1 = ObjectArray::FindStructFast("Guid").GetChildProperties();
	const UEFField GuidChild2 = GuidChild1.GetNext();

	auto IsPotentiallyValidOffset = [](int32 Offset) -> bool
		{
			// Make sure 0x4 aligned Offsets are neither the start, nor the middle of a pointer-member. Irrelevant for 32-bit, because the 2nd check will be 0x2 aligned then.
			return Offset != Off::FField::Class && Offset != (Off::FField::Class + (sizeof(void*) / 2))
				&& Offset != Off::FField::Next && Offset != (Off::FField::Next + (sizeof(void*) / 2))
				&& Offset != Off::FField::Vft && Offset != (Off::FField::Vft + (sizeof(void*) / 2))
				&& Offset != Off::FField::Name && Offset != (Off::FField::Name + Off::InSDK::Name::FNameSize);
		};

	int32 StartingOffset = 0x8;

	// Only pay attention to the 0x8 aligned size-options of FName, since the pair in the TMap is 0x8 aligned because of FString
	struct alignas(0x4) Name08Byte { uint8 Pad[0x08]; };
	struct alignas(0x4) Name16Byte { uint8 Pad[0x10]; };

	static auto AreValidMetadataMaps = []<typename NameType>(const TMap<NameType, FString>* MetadataMap1, const TMap<NameType, FString>* MetadataMap2)
	{
		if (!MetadataMap1->IsValid() || !MetadataMap2->IsValid())
			return false;

		const FString& Value1 = MetadataMap1->operator[](0).Value();
		const FString& Value2 = MetadataMap2->operator[](0).Value();

		return Value1.IsValid() && Value2.IsValid();
	};

	while (true)
	{
		if (!IsPotentiallyValidOffset(StartingOffset))
		{
			StartingOffset += sizeof(void*);
			continue;
		}

		const int32 Offset = GetValidPointerOffset<false>(GuidChild1.GetAddress(), GuidChild2.GetAddress(), StartingOffset, 0x40);
		StartingOffset = Offset + sizeof(void*);

		if (Offset == OffsetNotFound)
			break;

		if (!IsPotentiallyValidOffset(Offset))
			continue;

		const TMap<Name08Byte, FString>* PossibleMetaDataPtr1 = *reinterpret_cast<TMap<Name08Byte, FString>**>(reinterpret_cast<uintptr_t>(GuidChild1.GetAddress()) + Offset);
		const TMap<Name08Byte, FString>* PossibleMetaDataPtr2 = *reinterpret_cast<TMap<Name08Byte, FString>**>(reinterpret_cast<uintptr_t>(GuidChild2.GetAddress()) + Offset);

		if (!PossibleMetaDataPtr1 || !PossibleMetaDataPtr2 || Platform::IsBadReadPtr(PossibleMetaDataPtr1) || Platform::IsBadReadPtr(PossibleMetaDataPtr2))
			continue;

		if (!PossibleMetaDataPtr1->IsValid() || !PossibleMetaDataPtr2->IsValid())
			continue;

		if (PossibleMetaDataPtr1->Num() <= 0 || PossibleMetaDataPtr2->Num() <= 0)
			continue;

		if (PossibleMetaDataPtr1->Num() >= 0x10 || PossibleMetaDataPtr2->Num() >= 0x10)
			continue;

		auto GetDataPtrOfArrayInMap = [](const auto& Map) -> const void*
		{
			// TMap data is stored at offset 0x0, this is a hacky way to get the TArray::Data member of the map
			return *reinterpret_cast<const void* const*>(&Map);
		};

		if (Platform::IsBadReadPtr(GetDataPtrOfArrayInMap(PossibleMetaDataPtr1)) || Platform::IsBadReadPtr(GetDataPtrOfArrayInMap(PossibleMetaDataPtr2)))
			continue;

		if (Off::InSDK::Name::FNameSize <= 0x8)
		{
			if (AreValidMetadataMaps(PossibleMetaDataPtr1, PossibleMetaDataPtr2))
				return Offset;
		}
		else
		{
			if (AreValidMetadataMaps(reinterpret_cast<const TMap<Name16Byte, FString>*>(PossibleMetaDataPtr1), reinterpret_cast<const TMap<Name16Byte, FString>*>(PossibleMetaDataPtr1)))
				return Offset;
		}
	}

	return OffsetNotFound;
}

/* FField::ClassPrivate points at a static FFieldClass inside the module. The first members of FGuid and
   FIntPoint are both IntProperty, so they hold the same pointer there, and FColor's (a ByteProperty)
   holds another; the owner slot next to it points at three different structs. */
int32_t OffsetFinder::FindFFieldClassOffset()
{
	const uint8* Members[3] = {};
	const char* const StructNames[3] = { "Guid", "IntPoint", "Color" };

	for (int i = 0; i < 3; i++)
	{
		const UEStruct Struct = ObjectArray::FindStructFast(StructNames[i]);
		Members[i] = Struct ? static_cast<const uint8*>(Struct.GetChildProperties().GetAddress()) : nullptr;

		if (!Members[i])
			return OffsetNotFound;
	}

	for (int32 Offset = sizeof(void*); Offset <= 0x30; Offset += sizeof(void*))
	{
		const void* const IntClass = *reinterpret_cast<void* const*>(Members[0] + Offset);
		const void* const ByteClass = *reinterpret_cast<void* const*>(Members[2] + Offset);

		if (IntClass && IntClass == *reinterpret_cast<void* const*>(Members[1] + Offset) && IntClass != ByteClass
			&& Platform::IsAddressInProcessRange(IntClass) && Platform::IsAddressInProcessRange(ByteClass))
			return Offset;
	}

	return OffsetNotFound;
}

// This function assumes that the EnumObj passed in is valid and that the values of the enum are starting at 0
void InializeUEnumSettings(const void* EnumObj, const uint32_t UEnumNumValuesOffset)
{
	constexpr uintptr_t UE5EnumDynamicAllocationTag = 0x1;

	{
		// On UE5.6+ there are two arrays, one for just the FName*/UTF8Char* and one for just int64* values. Check if the array before NumValues contains just Values or TPair<Name, Value>.
		const uintptr_t PossibleValueArrayTaggedPtr = *reinterpret_cast<uintptr_t*>(reinterpret_cast<uintptr_t>(EnumObj) + UEnumNumValuesOffset - sizeof(void*));
		const int64* PossibleValueArrayPtr = reinterpret_cast<const int64*>(PossibleValueArrayTaggedPtr & ~UE5EnumDynamicAllocationTag);

		if (!Platform::IsBadReadPtr(PossibleValueArrayPtr) && !Platform::IsBadReadPtr(PossibleValueArrayPtr + 1) && !Platform::IsBadReadPtr(PossibleValueArrayPtr + 2))
		{
			if (PossibleValueArrayPtr[0] == 0 && PossibleValueArrayPtr[1] == 1 && PossibleValueArrayPtr[2] == 2)
			{
				Settings::Internal::bIsNewUE5EnumNamesContainer = true;
				return;
			}
		}
	}

	using ValueType = std::conditional_t<sizeof(void*) == 0x8, int64, int32>;
	struct Name08Byte { uint8 Pad[0x08]; };
	struct Name16Byte { uint8 Pad[0x10]; };

	const uint8* ArrayAddress = static_cast<const uint8*>(EnumObj) + UEnumNumValuesOffset - 0x8;

	auto InitEnumSettings = []<typename NameType>(const TArray<TPair<NameType, ValueType>>&ArrayOfNameValuePairs)
	{
		if (ArrayOfNameValuePairs[1].Second == 1)
			return;

		if constexpr (Settings::EngineCore::bCheckEnumNamesInUEnum)
		{
			if (static_cast<uint8_t>(ArrayOfNameValuePairs[1].Second) == 1 && static_cast<uint8_t>(ArrayOfNameValuePairs[2].Second) == 2)
			{

				Settings::Internal::bIsSmallEnumValue = true;
				return;
			}
		}

		Settings::Internal::bIsEnumNameOnly = true;
	};


	if (Settings::Internal::bUseCasePreservingName)
	{
		InitEnumSettings(*reinterpret_cast<const TArray<TPair<Name16Byte, ValueType>>*>(ArrayAddress));
	}
	else
	{
		InitEnumSettings(*reinterpret_cast<const TArray<TPair<Name08Byte, ValueType>>*>(ArrayAddress));
	}
}

/* FFieldClass */

/* The field classes of an IntProperty (FGuid's first member) and a ByteProperty (FColor's), which every
   FFieldClass finder below reads. */
static std::pair<uint8*, uint8*> IntAndByteFieldClasses()
{
	const UEStruct Guid = ObjectArray::FindStructFast("Guid");
	const UEStruct Color = ObjectArray::FindStructFast("Color");

	if (!Guid || !Color || !Guid.GetChildProperties() || !Color.GetChildProperties())
		return { nullptr, nullptr };

	return { static_cast<uint8*>(Guid.GetChildProperties().GetClass().GetAddress()), static_cast<uint8*>(Color.GetChildProperties().GetClass().GetAddress()) };
}

int32_t OffsetFinder::FindFieldClassCastFlagsOffset()
{
	const auto [IntClass, ByteClass] = IntAndByteFieldClasses();

	if (!IntClass || !ByteClass)
		return OffsetNotFound;

	std::vector<std::pair<void*, EClassCastFlags>> Infos;
	Infos.push_back({ IntClass,  EClassCastFlags::Field | EClassCastFlags::Property | EClassCastFlags::NumericProperty | EClassCastFlags::IntProperty  });
	Infos.push_back({ ByteClass, EClassCastFlags::Field | EClassCastFlags::Property | EClassCastFlags::NumericProperty | EClassCastFlags::ByteProperty });

	return FindOffset(Infos, sizeof(void*), 0x30);
}

/* FFieldClass::Id is the class's own cast-flag bit (DECLARE_FIELD's StaticClassCastFlagsPrivate), and
   comes before CastFlags, which holds that bit together with all of its supers'. */
int32_t OffsetFinder::FindFieldClassIdOffset()
{
	const auto [IntClass, ByteClass] = IntAndByteFieldClasses();

	if (!IntClass || !ByteClass || Off::FFieldClass::CastFlags == OffsetNotFound)
		return OffsetNotFound;

	std::vector<std::pair<void*, EClassCastFlags>> Infos;
	Infos.push_back({ IntClass, EClassCastFlags::IntProperty });
	Infos.push_back({ ByteClass, EClassCastFlags::ByteProperty });

	return FindOffset(Infos, sizeof(void*), Off::FFieldClass::CastFlags);
}

/* IntProperty and ByteProperty both derive from NumericProperty: SuperClass is the slot after CastFlags
   where both field classes point at the same FFieldClass, one whose own Id is NumericProperty's bit. */
int32_t OffsetFinder::FindFieldClassSuperClassOffset()
{
	const auto [IntClass, ByteClass] = IntAndByteFieldClasses();

	if (!IntClass || !ByteClass || Off::FFieldClass::CastFlags == OffsetNotFound || Off::FFieldClass::Id == OffsetNotFound)
		return OffsetNotFound;

	for (int32 Offset = Off::FFieldClass::CastFlags + sizeof(uint64); Offset <= Off::FFieldClass::CastFlags + 0x20; Offset += sizeof(void*))
	{
		const uint8* const Super = *reinterpret_cast<const uint8* const*>(IntClass + Offset);

		if (!Super || Super != *reinterpret_cast<const uint8* const*>(ByteClass + Offset) || Platform::IsBadReadPtr(Super + Off::FFieldClass::Id))
			continue;

		if (*reinterpret_cast<const EClassCastFlags*>(Super + Off::FFieldClass::Id) == EClassCastFlags::NumericProperty)
			return Offset;
	}

	return OffsetNotFound;
}

/* UEnum */
int32_t OffsetFinder::FindEnumNamesOffset()
{
	std::vector<std::pair<void*, int32_t>> Infos;

	Infos.push_back({ ObjectArray::FindObjectFast("ENetRole", EClassCastFlags::Enum).GetAddress(), 0x5 });
	Infos.push_back({ ObjectArray::FindObjectFast("ETraceTypeQuery", EClassCastFlags::Enum).GetAddress(), 0x22 });

	int UEnumNumValuesOffset = FindOffset(Infos);

	if (UEnumNumValuesOffset == OffsetNotFound)
	{
		Infos[0] = { ObjectArray::FindObjectFast("EAlphaBlendOption", EClassCastFlags::Enum).GetAddress(), 0x10 };
		Infos[1] = { ObjectArray::FindObjectFast("EUpdateRateShiftBucket", EClassCastFlags::Enum).GetAddress(), 0x8 };

		UEnumNumValuesOffset = FindOffset(Infos);
	}

	if (UEnumNumValuesOffset == OffsetNotFound)
		return OffsetNotFound;

	InializeUEnumSettings(Infos[0].first ? Infos[0].first : Infos[1].first, UEnumNumValuesOffset);

	return UEnumNumValuesOffset - sizeof(void*);
}

int32_t OffsetFinder::FindEnumUnderlayingTypeOffset()
{
	std::vector<std::pair<void*, UEEnum::EUnderlyingType>> Infos;
	Infos.push_back({ ObjectArray::FindObjectFast("ENetRole", EClassCastFlags::Enum).GetAddress(), UEEnum::EUnderlyingType::uint8 });
	Infos.push_back({ ObjectArray::FindObjectFast("ETraceTypeQuery", EClassCastFlags::Enum).GetAddress(), UEEnum::EUnderlyingType::uint8 });

	int UEnumUnderlayingTypeOffset = FindOffset(Infos, OffsetFinderMinValue, 0xA0);

	if (UEnumUnderlayingTypeOffset == OffsetNotFound)
	{
		Infos[0] = { ObjectArray::FindObjectFast("EAlphaBlendOption", EClassCastFlags::Enum).GetAddress(), UEEnum::EUnderlyingType::uint8 };
		Infos[1] = { ObjectArray::FindObjectFast("EUpdateRateShiftBucket", EClassCastFlags::Enum).GetAddress(), UEEnum::EUnderlyingType::uint8 };

		UEnumUnderlayingTypeOffset = FindOffset(Infos, OffsetFinderMinValue, 0xA0);
	}

	Settings::Internal::bHasUnderlayingTypeInUEnum = UEnumUnderlayingTypeOffset != OffsetNotFound;

	return UEnumUnderlayingTypeOffset;
}

/* UStruct */
int32_t OffsetFinder::FindSuperOffset()
{
	std::vector<std::pair<void*, void*>> Infos;

	Infos.push_back({ ObjectArray::FindObjectFast("Struct").GetAddress(), ObjectArray::FindObjectFast("Field").GetAddress() });
	Infos.push_back({ ObjectArray::FindObjectFast("Class").GetAddress(), ObjectArray::FindObjectFast("Struct").GetAddress() });

	// Thanks to the ue4 dev who decided UStruct should be spelled Ustruct
	if (Infos[0].first == nullptr)
		Infos[0].first = Infos[1].second = ObjectArray::FindObjectFast("struct").GetAddress();

	return FindOffset(Infos);
}

int32_t OffsetFinder::FindChildOffset()
{
	std::vector<std::pair<void*, void*>> Infos;

	if (ObjectArray::FindObject("ObjectProperty Engine.Controller.TransformComponent", EClassCastFlags::ObjectProperty))
	{
		Infos.push_back({ ObjectArray::FindObjectFast("Vector").GetAddress(), ObjectArray::FindObjectFastInOuter("X", "Vector").GetAddress() });
		Infos.push_back({ ObjectArray::FindObjectFast("Vector4").GetAddress(), ObjectArray::FindObjectFastInOuter("X", "Vector4").GetAddress() });
		Infos.push_back({ ObjectArray::FindObjectFast("Vector2D").GetAddress(), ObjectArray::FindObjectFastInOuter("X", "Vector2D").GetAddress() });
		Infos.push_back({ ObjectArray::FindObjectFast("Guid").GetAddress(), ObjectArray::FindObjectFastInOuter("A","Guid").GetAddress() });

		return FindOffset(Infos, 0x14);
	}

	Infos.push_back({ ObjectArray::FindObjectFast("PlayerController").GetAddress(), ObjectArray::FindObjectFastInOuter("WasInputKeyJustReleased", "PlayerController").GetAddress() });
	Infos.push_back({ ObjectArray::FindObjectFast("Controller").GetAddress(), ObjectArray::FindObjectFastInOuter("UnPossess", "Controller").GetAddress() });

	Settings::Internal::bUseFProperty = true;

	return FindOffset(Infos);
}

int32_t OffsetFinder::FindChildPropertiesOffset()
{
	const void* ObjA = ObjectArray::FindStructFast("Color").GetAddress();
	const void* ObjB = ObjectArray::FindStructFast("Guid").GetAddress();

	return GetValidPointerOffset(ObjA, ObjB, Off::UStruct::Children + 0x08, 0x80);
}

int32_t OffsetFinder::FindStructSizeOffset()
{
	std::vector<std::pair<void*, int32_t>> Infos;

	Infos.push_back({ ObjectArray::FindObjectFast("Color").GetAddress(), 0x04 });
	Infos.push_back({ ObjectArray::FindObjectFast("Guid").GetAddress(), 0x10 });

	return FindOffset(Infos);
}

int32_t OffsetFinder::FindMinAlignmentOffset()
{
	std::vector<std::pair<void*, int16_t>> Infos;

	Infos.push_back({ ObjectArray::FindObjectFast("Transform").GetAddress(), 0x10 });

	if constexpr (Platform::Is32Bit())
	{
		Infos.push_back({ ObjectArray::FindObjectFast("InterpCurveLinearColor").GetAddress(), 0x04 });
	}
	else
	{
		Infos.push_back({ ObjectArray::FindObjectFast("PlayerController").GetAddress(), 0x8 });
	}

	return FindOffset(Infos);
}

int32_t OffsetFinder::FindScriptOffset()
{
	/* UStruct::Script (TArray<uint8> Blueprint bytecode) sits immediately after MinAlignment (int32) across
	   UE4.15-5.x. Derive from the binary-found MinAlignment anchor, then verify it against the scripts of the
	   game's Blueprint functions. Dumper-7 did not emit this before; the BP-VM disassembler needs it. */
	if (Off::UStruct::MinAlignment == OffsetNotFound)
		return OffsetNotFound;

	const int32 Candidate = Align(Off::UStruct::MinAlignment + static_cast<int32>(sizeof(int32)), static_cast<int32>(sizeof(void*)));

	/* A compiled Kismet script ends in EX_EndOfScript (0x53, the same on UE4 and UE5). Nearly every scripted
	   function holding a non-empty array there must hold such a script - not all, as a mod loader may patch
	   some - while at a wrong offset next to none would. Native functions are left out. */
	constexpr uint8 EX_EndOfScript = 0x53;

	int32 NumArrays = 0;
	int32 NumScripts = 0;

	for (UEObject Obj : ObjectArray())
	{
		if (!Obj.IsA(EClassCastFlags::Function) || Obj.Cast<UEFunction>().HasFlags(EFunctionFlags::Native))
			continue;

		const uint8* const Addr = static_cast<const uint8*>(Obj.GetAddress());
		const uint8* const ScriptData = *reinterpret_cast<const uint8* const*>(Addr + Candidate);
		const int32 ScriptNum = *reinterpret_cast<const int32*>(Addr + Candidate + 0x08);
		const int32 ScriptMax = *reinterpret_cast<const int32*>(Addr + Candidate + 0x0C);

		if (ScriptNum == 0)
			continue;

		NumArrays++;

		NumScripts += ScriptNum > 0 && ScriptMax >= ScriptNum && !Platform::IsBadReadPtr(ScriptData)
			&& !Platform::IsBadReadPtr(ScriptData + ScriptNum - 1) && ScriptData[ScriptNum - 1] == EX_EndOfScript;
	}

	std::cerr << std::format("UStruct::Script candidate 0x{:X}: {} of {} non-empty arrays end in EX_EndOfScript\n", Candidate, NumScripts, NumArrays);

	return NumScripts > 0 && NumScripts * 100 >= NumArrays * 95 ? Candidate : OffsetNotFound;
}

int32_t OffsetFinder::FindStructBaseChainOffset()
{
	/* UStruct embeds FStructBaseChain { FStructBaseChain** StructBaseChainArray; int32 NumStructBasesInChainMinusOne; }
	   as its first own members (UE4.22+). Defining invariant: StructBaseChainArray[NumStructBasesInChainMinusOne]
	   points back to this struct's own FStructBaseChain subobject (== (uint8*)Struct + Offset). Scan the UStruct
	   region for the (void**, int32) pair satisfying that self-at-chain-end invariant, validated across a few
	   classes of differing inheritance depth. Powers an O(1) IsChildOf; absent on pre-UE4.22 -> OffsetNotFound. */
	void* const Structs[] = {
		ObjectArray::FindObjectFast("Object").GetAddress(),  // depth 0 (no super)
		ObjectArray::FindObjectFast("Field").GetAddress(),   // depth 1 (Object)
		ObjectArray::FindObjectFast("Struct").GetAddress(),  // depth 2 (Object, Field)
	};

	auto SatisfiesInvariant = [](void* StructPtr, int32 Offset) -> bool
	{
		if (StructPtr == nullptr)
			return false;

		uint8* const Base = static_cast<uint8*>(StructPtr);
		void** const Array = *reinterpret_cast<void** const*>(Base + Offset);
		const int32 NumMinusOne = *reinterpret_cast<const int32*>(Base + Offset + sizeof(void*));

		if (Platform::IsBadReadPtr(Array) || NumMinusOne < 0 || NumMinusOne > 0x40)
			return false;

		if (Platform::IsBadReadPtr(reinterpret_cast<void*>(Array + NumMinusOne)))
			return false;

		// the chain ends with this struct's own FStructBaseChain subobject (== Base + Offset)
		return Array[NumMinusOne] == reinterpret_cast<void*>(Base + Offset);
	};

	const int32 MaxOffset = Off::UStruct::SuperStruct != 0x0 ? Off::UStruct::SuperStruct : 0x50;

	for (int32 Offset = OffsetFinderMinValue; Offset < MaxOffset; Offset += sizeof(void*))
	{
		bool bAllValid = true;
		int32 NumChecked = 0;

		for (void* StructPtr : Structs)
		{
			if (StructPtr == nullptr)
				continue;

			++NumChecked;
			if (!SatisfiesInvariant(StructPtr, Offset))
			{
				bAllValid = false;
				break;
			}
		}

		if (bAllValid && NumChecked > 0)
			return Offset;
	}

	return OffsetNotFound;
}

/* UFunction */
int32_t OffsetFinder::FindFunctionFlagsOffset()
{
	std::vector<std::pair<void*, EFunctionFlags>> Infos;

	Infos.push_back({ ObjectArray::FindObjectFast("WasInputKeyJustPressed", EClassCastFlags::Function).GetAddress(), EFunctionFlags::Final | EFunctionFlags::Native | EFunctionFlags::Public | EFunctionFlags::BlueprintCallable | EFunctionFlags::BlueprintPure | EFunctionFlags::Const });
	Infos.push_back({ ObjectArray::FindObjectFast("ToggleSpeaking", EClassCastFlags::Function).GetAddress(), EFunctionFlags::Exec | EFunctionFlags::Native | EFunctionFlags::Public });
	Infos.push_back({ ObjectArray::FindObjectFast("SwitchLevel", EClassCastFlags::Function).GetAddress(), EFunctionFlags::Exec | EFunctionFlags::Native | EFunctionFlags::Public });

	// Some games don't have APlayerController::SwitchLevel(), so we replace it with APlayerController::FOV() which has the same FunctionFlags
	if (Infos[2].first == nullptr)
		Infos[2].first = ObjectArray::FindObjectFast("FOV", EClassCastFlags::Function).GetAddress();

	const int32 Ret = FindOffset(Infos);

	if (Ret != OffsetNotFound)
		return Ret;

	for (auto& [_, Flags] : Infos)
		Flags |= EFunctionFlags::RequiredAPI;

	return FindOffset(Infos);
}

int32_t OffsetFinder::FindFunctionNativeFuncOffset()
{
	std::vector<std::pair<void*, EFunctionFlags>> Infos;

	uintptr_t WasInputKeyJustPressed = reinterpret_cast<uintptr_t>(ObjectArray::FindObjectFast("WasInputKeyJustPressed", EClassCastFlags::Function).GetAddress());
	uintptr_t ToggleSpeaking = reinterpret_cast<uintptr_t>(ObjectArray::FindObjectFast("ToggleSpeaking", EClassCastFlags::Function).GetAddress());
	uintptr_t SwitchLevel_Or_FOV = reinterpret_cast<uintptr_t>(ObjectArray::FindObjectFast("SwitchLevel", EClassCastFlags::Function).GetAddress());

	// Some games don't have APlayerController::SwitchLevel(), so we replace it with APlayerController::FOV() which has the same FunctionFlags
	if (SwitchLevel_Or_FOV == NULL)
		SwitchLevel_Or_FOV = reinterpret_cast<uintptr_t>(ObjectArray::FindObjectFast("FOV", EClassCastFlags::Function).GetAddress());

	if (!WasInputKeyJustPressed || !ToggleSpeaking || !SwitchLevel_Or_FOV)
		return OffsetNotFound;

	for (int i = 0x30; i < 0x140; i += sizeof(void*))
	{
		if (Platform::IsAddressInProcessRange(*reinterpret_cast<uintptr_t*>(WasInputKeyJustPressed + i)) &&
			Platform::IsAddressInProcessRange(*reinterpret_cast<uintptr_t*>(ToggleSpeaking + i)) && Platform::IsAddressInProcessRange(*reinterpret_cast<uintptr_t*>(SwitchLevel_Or_FOV + i)))
			return i;
	}

	return OffsetNotFound;
}

/* UClass */
int32_t OffsetFinder::FindCastFlagsOffset()
{
	std::vector<std::pair<void*, EClassCastFlags>> Infos;

	Infos.push_back({ ObjectArray::FindObjectFast("Actor").GetAddress(), EClassCastFlags::Actor });
	Infos.push_back({ ObjectArray::FindObjectFast("Class").GetAddress(), EClassCastFlags::Field | EClassCastFlags::Struct | EClassCastFlags::Class });

	return FindOffset(Infos);
}

int32_t OffsetFinder::FindDefaultObjectOffset()
{
	std::vector<std::pair<void*, void*>> Infos;

	Infos.push_back({ ObjectArray::FindClassFast("Object").GetAddress(), ObjectArray::FindObjectFast("Default__Object").GetAddress() });
	Infos.push_back({ ObjectArray::FindClassFast("Field").GetAddress(), ObjectArray::FindObjectFast("Default__Field").GetAddress() });

	return FindOffset(Infos, 0x28, 0x200);
}

int32_t OffsetFinder::FindImplementedInterfacesOffset()
{
	UEClass Interface_AssetUserDataClass = ObjectArray::FindClassFast("Interface_AssetUserData");

	const uint8_t* ActorComponentClassPtr = reinterpret_cast<const uint8_t*>(ObjectArray::FindClassFast("ActorComponent").GetAddress());

	if (!Interface_AssetUserDataClass || !ActorComponentClassPtr)
		return OffsetNotFound;

	for (int i = Off::UClass::ClassDefaultObject; i <= (0x350 - 0x10); i += sizeof(void*))
	{
		const auto& ActorArray = *reinterpret_cast<const TArray<FImplementedInterface>*>(ActorComponentClassPtr + i);

		if (ActorArray.IsValid() && !Platform::IsBadReadPtr(ActorArray.GetDataPtr()))
		{
			if (ActorArray[0].InterfaceClass == Interface_AssetUserDataClass)
				return i;
		}
	}

	return OffsetNotFound;
}

/* Property */
int32_t OffsetFinder::FindElementSizeOffset()
{
	std::vector<std::pair<void*, int32_t>> Infos;

	UEStruct Guid = ObjectArray::FindStructFast("Guid");

	Infos.push_back({ Guid.FindMember("A").GetAddress(), 0x04 });
	Infos.push_back({ Guid.FindMember("C").GetAddress(), 0x04 });
	Infos.push_back({ Guid.FindMember("D").GetAddress(), 0x04 });

	return FindOffset(Infos);
}

int32_t OffsetFinder::FindArrayDimOffset()
{
	std::vector<std::pair<void*, int32_t>> Infos;

	UEStruct Guid = ObjectArray::FindStructFast("Guid");

	Infos.push_back({ Guid.FindMember("A").GetAddress(), 0x01 });
	Infos.push_back({ Guid.FindMember("C").GetAddress(), 0x01 });
	Infos.push_back({ Guid.FindMember("D").GetAddress(), 0x01 });

	const int32_t MinOffset = Off::Property::ElementSize - 0x10;
	const int32_t MaxOffset = Off::Property::ElementSize + 0x10;

	return FindOffset(Infos, MinOffset, MaxOffset);
}

int32_t OffsetFinder::FindPropertyFlagsOffset()
{
	std::vector<std::pair<void*, EPropertyFlags>> Infos;


	UEStruct Guid = ObjectArray::FindStructFast("Guid");
	UEStruct Color = ObjectArray::FindStructFast("Color");

	constexpr EPropertyFlags GuidMemberFlags = EPropertyFlags::Edit | EPropertyFlags::ZeroConstructor | EPropertyFlags::SaveGame | EPropertyFlags::IsPlainOldData | EPropertyFlags::NoDestructor | EPropertyFlags::HasGetValueTypeHash;
	constexpr EPropertyFlags ColorMemberFlags = EPropertyFlags::Edit | EPropertyFlags::BlueprintVisible | EPropertyFlags::ZeroConstructor | EPropertyFlags::SaveGame | EPropertyFlags::IsPlainOldData | EPropertyFlags::NoDestructor | EPropertyFlags::HasGetValueTypeHash;

	Infos.push_back({ Guid.FindMember("A").GetAddress(), GuidMemberFlags });
	Infos.push_back({ Color.FindMember("R").GetAddress(), ColorMemberFlags });

	if (Infos[1].first == nullptr) [[unlikely]]
		Infos[1].first = Color.FindMember("r").GetAddress();

	int FlagsOffset = FindOffset(Infos);

	// Same flags without AccessSpecifier
	if (FlagsOffset == OffsetNotFound)
	{
		Infos[0].second |= EPropertyFlags::NativeAccessSpecifierPublic;
		Infos[1].second |= EPropertyFlags::NativeAccessSpecifierPublic;

		FlagsOffset = FindOffset(Infos);
	}

	return FlagsOffset;
}

int32_t OffsetFinder::FindOffsetInternalOffset()
{
	std::vector<std::pair<void*, int32_t>> Infos;

	const UEStruct Color = ObjectArray::FindStructFast("Color");
	const UEStruct Guid = ObjectArray::FindStructFast("Guid");

	Infos.push_back({ Color.FindMember("B").GetAddress(), 0x00 });
	Infos.push_back({ Color.FindMember("G").GetAddress(), 0x01 });
	Infos.push_back({ Guid.FindMember("C").GetAddress(), 0x08 });

	// Thanks to the ue5 dev who decided FColor::R should be spelled FColor::r
	if (Infos[2].first == nullptr) [[unlikely]]
		Infos[2].first = Color.FindMember("r").GetAddress();

	return FindOffset(Infos);
}

/* BoolProperty */
int32_t OffsetFinder::FindBoolPropertyBaseOffset()
{
	std::vector<std::pair<void*, uint8_t>> Infos;

	UEClass Engine = ObjectArray::FindClassFast("Engine");
	Infos.push_back({ Engine.FindMember("bIsOverridingSelectedColor").GetAddress(), 0xFF });
	Infos.push_back({ Engine.FindMember("bEnableOnScreenDebugMessagesDisplay").GetAddress(), 0b00000010 });
	Infos.push_back({ ObjectArray::FindClassFast("PlayerController").FindMember("bAutoManageActiveCameraTarget").GetAddress(), 0xFF });

	/* The match is FieldMask, the last of the four bytes. */
	const int32 FieldMaskOffset = FindOffset<1>(Infos, Off::Property::Offset_Internal);

	return FieldMaskOffset != OffsetNotFound ? FieldMaskOffset - 0x3 : OffsetNotFound;
}

/* ObjectPrperty */
int32_t OffsetFinder::FindObjectPropertyClassOffset(int32 PropertySize)
{
	std::vector<std::pair<void*, void*>> Infos;

	const UEClass Controller = ObjectArray::FindClassFast("Controller");
	Infos.push_back({ Controller.FindMember("PlayerState").GetAddress(), ObjectArray::FindClassFast("PlayerState").GetAddress() });
	Infos.push_back({ Controller.FindMember("Pawn").GetAddress(), ObjectArray::FindClassFast("Pawn").GetAddress() });
	Infos.push_back({ ObjectArray::FindClassFast("World").FindMember("PersistentLevel").GetAddress(), ObjectArray::FindClassFast("Level").GetAddress() });

	const int32 Offset = FindOffset(Infos, Off::Property::Offset_Internal);

	if (Offset != OffsetNotFound)
		return Offset;

	/* PropertyClass is the first member FObjectPropertyBase adds, so it sits at PropertySize when it holds
	   a class in every ObjectProperty sampled. */
	return HoldObjectsOfType(SampleProperties(EClassCastFlags::ObjectProperty, 8), PropertySize, EClassCastFlags::Class) ? PropertySize : OffsetNotFound;
}

/* EnumProperty */
int32_t OffsetFinder::FindEnumPropertyBaseOffset()
{
	std::vector<std::pair<void*, const void*>> Infos;

	const void* ComponentCreationMethod = ObjectArray::FindObjectFast("EComponentCreationMethod", EClassCastFlags::Enum).GetAddress();
	const void* AutoPossessAI = ObjectArray::FindObjectFast("EAutoPossessAI", EClassCastFlags::Enum).GetAddress();

	if (!ComponentCreationMethod || !AutoPossessAI)
		return OffsetNotFound;

	void* CreationMethodMember = ObjectArray::FindClassFast("ActorComponent").FindMember("CreationMethod", EClassCastFlags::EnumProperty).GetAddress();
	void* AutoPossessAIMember = ObjectArray::FindClassFast("Pawn").FindMember("AutoPossessAI", EClassCastFlags::EnumProperty).GetAddress();

	// UE4.15 and below don't have EnumProperty
	if (!CreationMethodMember || !AutoPossessAIMember)
		return OffsetNotFound;

	Infos.push_back({ CreationMethodMember, ComponentCreationMethod });
	Infos.push_back({ AutoPossessAIMember , AutoPossessAI });

	// EnumProperty::Enum is the 2nd member after 'NumericProperty UnderlyingType'
	const int32 EnumOffset = FindOffset(Infos, Off::Property::Offset_Internal);

	if (EnumOffset == OffsetNotFound)
		return OffsetNotFound;

	/* ...and that UnderlyingType is a property owned by the EnumProperty itself. */
	const int32 Base = EnumOffset - sizeof(void*);

	for (const void* Member : { CreationMethodMember, AutoPossessAIMember })
	{
		if (!IsPropertyOwnedBy(*reinterpret_cast<void* const*>(static_cast<const uint8*>(Member) + Base), Member))
			return OffsetNotFound;
	}

	return Base;
}

/* ByteProperty */
int32_t OffsetFinder::FindBytePropertyEnumOffset()
{
	std::vector<std::pair<void*, const void*>> Infos;

	const void* CollisionResponseEnum = ObjectArray::FindObjectFast("ECollisionResponse", EClassCastFlags::Enum).GetAddress();

	const UEStruct CollisionResponseContainer = ObjectArray::FindStructFast("CollisionResponseContainer");

	if (!CollisionResponseEnum || !CollisionResponseContainer)
		return OffsetNotFound;

	const void* GameTraceChannel1 = CollisionResponseContainer.FindMember("GameTraceChannel1", EClassCastFlags::ByteProperty).GetAddress();
	const void* GameTraceChannel2 = CollisionResponseContainer.FindMember("GameTraceChannel2", EClassCastFlags::ByteProperty).GetAddress();

	if (!GameTraceChannel1 || !GameTraceChannel2)
		return OffsetNotFound;

	Infos.push_back({ const_cast<void*>(GameTraceChannel1), CollisionResponseEnum });
	Infos.push_back({ const_cast<void*>(GameTraceChannel2), CollisionResponseEnum });

	return FindOffset(Infos, Off::Property::Offset_Internal);
}

/* StructProperty */
int32_t OffsetFinder::FindStructPropertyStructOffset(int32 PropertySize)
{
	std::vector<std::pair<void*, const void*>> Infos;

	const void* VectorClass = ObjectArray::FindStructFast("Vector").GetAddress();

	if (VectorClass == nullptr)
		VectorClass = ObjectArray::FindClassFast("vector").GetAddress();

	const UEStruct TwoVectorsStruct = ObjectArray::FindStructFast("TwoVectors");

	const void* v1 = TwoVectorsStruct ? TwoVectorsStruct.FindMember("v1", EClassCastFlags::StructProperty).GetAddress() : nullptr;
	const void* v2 = TwoVectorsStruct ? TwoVectorsStruct.FindMember("v2", EClassCastFlags::StructProperty).GetAddress() : nullptr;

	if (VectorClass && v1 && v2)
	{
		Infos.push_back({ const_cast<void*>(v1), VectorClass });
		Infos.push_back({ const_cast<void*>(v2), VectorClass });

		if (const int32 Offset = FindOffset(Infos, Off::Property::Offset_Internal); Offset != OffsetNotFound)
			return Offset;
	}

	/* Struct is the first member FStructProperty adds. */
	return HoldObjectsOfType(SampleProperties(EClassCastFlags::StructProperty, 8), PropertySize, EClassCastFlags::Struct) ? PropertySize : OffsetNotFound;
}

/* DelegateProperty */
int32_t OffsetFinder::FindDelegatePropertySignatureFunctionOffset(int32 PropertySize)
{
	std::vector<std::pair<void*, const void*>> Infos;

	const void* DelegateSignature = ObjectArray::FindObjectFast("TimerDynamicDelegate__DelegateSignature", EClassCastFlags::Function).GetAddress();

	const UEFunction ElapsedTime = ObjectArray::FindObjectFast<UEFunction>("K2_GetTimerElapsedTimeDelegate", EClassCastFlags::Function);
	const UEFunction RemainingTime = ObjectArray::FindObjectFast<UEFunction>("K2_GetTimerRemainingTimeDelegate", EClassCastFlags::Function);

	const void* Delegate1 = ElapsedTime ? ElapsedTime.FindMember("Delegate", EClassCastFlags::DelegateProperty).GetAddress() : nullptr;
	const void* Delegate2 = RemainingTime ? RemainingTime.FindMember("Delegate", EClassCastFlags::DelegateProperty).GetAddress() : nullptr;

	if (DelegateSignature && Delegate1 && Delegate2)
	{
		Infos.push_back({ const_cast<void*>(Delegate1), DelegateSignature });
		Infos.push_back({ const_cast<void*>(Delegate2), DelegateSignature });

		if (const int32 Offset = FindOffset(Infos, Off::Property::Offset_Internal); Offset != OffsetNotFound)
			return Offset;
	}

	/* SignatureFunction is the first member FDelegateProperty adds. */
	return HoldObjectsOfType(SampleProperties(EClassCastFlags::DelegateProperty, 8), PropertySize, EClassCastFlags::Function) ? PropertySize : OffsetNotFound;
}

/* ArrayProperty */
int32_t OffsetFinder::FindInnerTypeOffset(const int32 PropertySize)
{
	return FindOwnedPropertySlot(EClassCastFlags::ArrayProperty, PropertySize, 1);
}

/* SetProperty */
int32_t OffsetFinder::FindSetPropertyBaseOffset(const int32 PropertySize)
{
	return FindOwnedPropertySlot(EClassCastFlags::SetProperty, PropertySize, 1);
}

/* MapProperty: the key property, with the value property right behind it. */
int32_t OffsetFinder::FindMapPropertyBaseOffset(const int32 PropertySize)
{
	return FindOwnedPropertySlot(EClassCastFlags::MapProperty, PropertySize, 2);
}

/*
RepIndex (uint16) and BlueprintReplicationCondition (uint8) are all that sits between PropertyFlags and
Offset_Internal on 4.25 through 5.6; an editor build adds IndexInOwner there, which the gap check
refuses. The engine numbers RepIndex only for replicated (CPF_Net) properties and leaves it 0 on the
rest, which is what Actor's other properties are checked for.
*/
int32 OffsetFinder::FindRepIndexOffset(int32 PropertyFlagsOffset, int32 OffsetInternalOffset)
{
	if (PropertyFlagsOffset == OffsetNotFound || OffsetInternalOffset == OffsetNotFound)
		return OffsetNotFound;

	const int32 Candidate = PropertyFlagsOffset + sizeof(uint64);

	if (OffsetInternalOffset - Candidate != sizeof(uint32))
		return OffsetNotFound;

	const UEClass Actor = ObjectArray::FindClassFast("Actor");
	if (!Actor)
		return OffsetNotFound;

	int32 NumChecked = 0;

	for (const UEProperty& Prop : Actor.GetProperties())
	{
		if (Prop.HasPropertyFlags(EPropertyFlags::Net))
			continue;

		if (*reinterpret_cast<const uint16*>(static_cast<const uint8*>(Prop.GetAddress()) + Candidate) != 0)
			return OffsetNotFound;

		NumChecked++;
	}

	return NumChecked > 0 ? Candidate : OffsetNotFound;
}

/* The ELifetimeCondition right after RepIndex: below COND_Max, which is under 0x20 on every version,
   for all of Actor's properties. */
int32 OffsetFinder::FindBlueprintReplicationConditionOffset(int32 RepIndexOffset)
{
	if (RepIndexOffset == OffsetNotFound)
		return OffsetNotFound;

	const int32 Candidate = RepIndexOffset + sizeof(uint16);
	const UEClass Actor = ObjectArray::FindClassFast("Actor");

	if (!Actor)
		return OffsetNotFound;

	for (const UEProperty& Prop : Actor.GetProperties())
	{
		if (*(static_cast<const uint8*>(Prop.GetAddress()) + Candidate) >= 0x20)
			return OffsetNotFound;
	}

	return Candidate;
}

/*
PropertyLinkNext is the one FProperty slot that points at a sibling: UStruct::Link chains a struct's
properties through it, so FGuid's four members link A -> B -> C -> D with exactly one null at the
end. It is found by that chain rather than counted from Offset_Internal, because UE5 moved
RepNotifyFunc from in front of the four link pointers to behind them (5.6 UnrealType.h). Counting
the UE4 way on a 5.6 build reads PropertyLinkNext as an FName, and the first RepNotify property
whose link pointer is not a valid name index crashed the game inside FName::ToString.
*/
int32 OffsetFinder::FindPropertyLinkNextOffset(const int32 OffsetInternalOffset)
{
	const UEStruct Guid = ObjectArray::FindStructFast("Guid");

	std::vector<uintptr_t> Members;
	for (const char* Name : { "A", "B", "C", "D" })
	{
		if (void* Member = Guid.FindMember(Name).GetAddress())
			Members.push_back(reinterpret_cast<uintptr_t>(Member));
	}

	if (Members.size() != 4)
		return OffsetNotFound;

	/* The UE5 slot comes first; the UE4 one sits past RepNotifyFunc. */
	const int32 First = Align(OffsetInternalOffset + static_cast<int32>(sizeof(int32)), static_cast<int32>(sizeof(void*)));
	const int32 Last = First + Off::InSDK::Name::FNameSize + static_cast<int32>(sizeof(void*));

	for (int32 Offset = First; Offset <= Last; Offset += sizeof(void*))
	{
		int32 Links = 0;
		int32 Nulls = 0;

		for (const uintptr_t Member : Members)
		{
			const uintptr_t Next = *reinterpret_cast<const uintptr_t*>(Member + Offset);

			Links += std::find(Members.begin(), Members.end(), Next) != Members.end();
			Nulls += Next == 0;
		}

		if (Links == 3 && Nulls == 1)
			return Offset;
	}

	return OffsetNotFound;
}

/*
RepNotifyFunc names the function the engine calls when a replicated property changes, and is None on
every other property. UE4 keeps it right behind Offset_Internal, UE5 behind the four link pointers; the
scan covers both. The slot is the one where every RepNotify property of the classes below names a
function of its class or a super, and every other property holds None - compared as name indices, so
no name is looked up while the offset is still a guess.
*/
struct FNameKey
{
	int32 CompIdx;
	uint32 Number;

	bool operator==(const FNameKey&) const = default;
};

int32 OffsetFinder::FindRepNotifyFuncOffset(int32 OffsetInternalOffset, int32 PropertySize)
{
	if (OffsetInternalOffset == OffsetNotFound || PropertySize == OffsetNotFound)
		return OffsetNotFound;

	auto ReadName = [](const void* Address) -> FNameKey
	{
		const FName Name(Address);
		return { Name.GetCompIdx(), Name.GetNumber() };
	};

	/* Each class's own properties, with the names of the functions it and its supers declare. */
	std::vector<std::pair<std::vector<UEProperty>, std::vector<FNameKey>>> Classes;

	for (const char* ClassName : { "Actor", "Pawn", "Character", "Controller", "PlayerState" })
	{
		const UEClass Class = ObjectArray::FindClassFast(ClassName);
		if (!Class)
			continue;

		std::vector<FNameKey> Functions;

		for (UEStruct Struct = Class; Struct; Struct = Struct.GetSuper())
		{
			for (const UEFunction& Func : Struct.GetFunctions())
				Functions.push_back(ReadName(Func.GetFName().GetAddress()));
		}

		Classes.emplace_back(Class.GetProperties(), std::move(Functions));
	}

	for (int32 Offset = Align(OffsetInternalOffset + static_cast<int32>(sizeof(int32)), 0x4); Offset + Off::InSDK::Name::FNameSize <= PropertySize; Offset += 0x4)
	{
		int32 NumNotifies = 0;
		bool bMatches = true;

		for (const auto& [Properties, Functions] : Classes)
		{
			for (const UEProperty& Prop : Properties)
			{
				const FNameKey Value = ReadName(static_cast<const uint8*>(Prop.GetAddress()) + Offset);

				if (!Prop.HasPropertyFlags(EPropertyFlags::RepNotify))
				{
					bMatches = bMatches && Value == FNameKey{ 0, 0 };
					continue;
				}

				bMatches = bMatches && std::find(Functions.begin(), Functions.end(), Value) != Functions.end();
				NumNotifies++;
			}
		}

		if (bMatches && NumNotifies >= 2)
			return Offset;
	}

	return OffsetNotFound;
}

/*
NextRef, DestructorLinkNext and PostConstructLinkNext follow PropertyLinkNext (ChainIndex 1, 2 and 3) on
4.25 through 5.6. Each links the properties of a struct that need one kind of handling (UStruct::Link):
object references, destruction - left to the native destructor in a native class, so a USTRUCT is
sampled - and CDO copies after construction, which a native class does for its config properties.
In the sample, the slot is null or points at another property of the same struct, at least once; in
FGuid, whose four ints need none of the three, it is null.
*/
int32 OffsetFinder::FindPropertyChainOffset(int32 PropertyLinkNextOffset, int32 ChainIndex)
{
	if (PropertyLinkNextOffset == OffsetNotFound)
		return OffsetNotFound;

	const int32 Offset = PropertyLinkNextOffset + ChainIndex * static_cast<int32>(sizeof(void*));

	const UEStruct Guid = ObjectArray::FindStructFast("Guid");
	const UEStruct Sample = ObjectArray::FindStructFast(ChainIndex == 2 ? "URL" : "Engine");

	if (!Guid || !Sample)
		return OffsetNotFound;

	for (const UEProperty& Prop : Guid.GetProperties())
	{
		if (ReadPtr(Prop, Offset) != nullptr)
			return OffsetNotFound;
	}

	int32 NumLinks = 0;

	for (const UEProperty& Prop : Sample.GetProperties())
	{
		const void* const Next = ReadPtr(Prop, Offset);

		if (!Next)
			continue;

		if (!IsPropertyOwnedBy(Next, Sample.GetAddress()))
			return OffsetNotFound;

		NumLinks++;
	}

	return NumLinks > 0 ? Offset : OffsetNotFound;
}

/* InSDK -> ULevel */
int32 OffsetFinder::FindLevelActorsOffset()
{
	UEObject Level = nullptr;
	uintptr_t Lvl = 0x0;

	for (auto Obj : ObjectArray())
	{
		if (Obj.HasAnyFlags(EObjectFlags::ClassDefaultObject) || !Obj.IsA(EClassCastFlags::Level))
			continue;

		Level = Obj;
		Lvl = reinterpret_cast<uintptr_t>(Obj.GetAddress());
		break;
	}

	if (Lvl == 0x0)
		return OffsetNotFound;

	/*
	class ULevel : public UObject
	{
		FURL URL;
		TArray<AActor*> Actors;
		TArray<AActor*> GCActors;
	};

	SearchStart = sizeof(UObject) + sizeof(FURL)
	SearchEnd = offsetof(ULevel, OwningWorld)
	*/
	UEClass UObjectClass = ObjectArray::FindClassFast("Object");
	if (!UObjectClass)
		UObjectClass = ObjectArray::FindClassFast("object");

	const UEStruct FURLStruct = ObjectArray::FindObjectFast<UEStruct>("URL", EClassCastFlags::Struct);

	const UEProperty Level_OwningWorldProperty = Level.GetClass().FindMember("OwningWorld");

	if (!UObjectClass || !FURLStruct || !Level_OwningWorldProperty)
		return OffsetNotFound;

	const int32 SearchStart = UObjectClass.GetStructSize() + FURLStruct.GetStructSize();
	const int32 SearchEnd = Level_OwningWorldProperty.GetOffset();

	/* The first array there whose first non-null element is a live Actor: the level's WorldSettings
	   (Actors[0]) or another actor, never something that merely looks like an array. */
	auto HoldsActors = [](const TArray<void*>& Array)
	{
		for (int32 i = 0; i < Array.Num() && i < 8; i++)
		{
			if (Array[i])
				return IsLiveObject(Array[i]) && UEObject(Array[i]).IsA(EClassCastFlags::Actor);
		}

		return false;
	};

	for (int i = SearchStart; i <= (SearchEnd - 0x10); i += sizeof(void*))
	{
		const TArray<void*>& ActorArray = *reinterpret_cast<TArray<void*>*>(Lvl + i);

		if (ActorArray.IsValid() && !Platform::IsBadReadPtr(ActorArray.GetDataPtr()) && HoldsActors(ActorArray))
			return i;
	}

	return OffsetNotFound;
}


/* InSDK -> UDataTable: RowMap follows the reflected RowStruct pointer. */
int32 OffsetFinder::FindDatatableRowMapOffset()
{
	const UEClass DataTable = ObjectArray::FindClassFast("DataTable");
	const UEProperty RowStructProp = DataTable ? DataTable.FindMember("RowStruct", EClassCastFlags::ObjectProperty) : UEProperty(nullptr);

	if (!RowStructProp)
		return OffsetNotFound;

	return RowStructProp.GetOffset() + RowStructProp.GetSize();
}

/* FieldPathProperty: PropertyClass, the FFieldClass a TFieldPath may point to - FField or one of its
   subclasses, so following SuperClass from it ends at the root of IntProperty's field-class chain. */
int32 OffsetFinder::FindFieldPathPropertyFieldClassOffset(int32 PropertySize)
{
	if (PropertySize == OffsetNotFound || Off::FFieldClass::SuperClass == OffsetNotFound)
		return OffsetNotFound;

	const uint8* const IntClass = IntAndByteFieldClasses().first;
	if (!IntClass)
		return OffsetNotFound;

	auto RootOf =[](const uint8* FieldClass) -> const uint8*
	{
		for (int i = 0; FieldClass && i < 0x10; i++)
		{
			if (Platform::IsBadReadPtr(FieldClass + Off::FFieldClass::SuperClass))
				return nullptr;

			const uint8* const Super = *reinterpret_cast<const uint8* const*>(FieldClass + Off::FFieldClass::SuperClass);

			if (!Super)
				return FieldClass;

			FieldClass = Super;
		}

		return nullptr;
	};

	const uint8* const FieldRoot = RootOf(IntClass);
	const std::vector<UEProperty> Samples = SampleProperties(EClassCastFlags::FieldPathProperty, 8);

	const bool bAllPointIntoFieldClasses = !Samples.empty() && FieldRoot && std::all_of(Samples.begin(), Samples.end(), [&](const UEProperty& Prop)
	{
		const uint8* const FieldClass = static_cast<const uint8*>(ReadPtr(Prop, PropertySize));
		return FieldClass && RootOf(FieldClass) == FieldRoot;
	});

	return bAllPointIntoFieldClasses ? PropertySize : OffsetNotFound;
}

/* OptionalProperty (UE5): ValueProperty, the property it wraps and owns. */
int32 OffsetFinder::FindOptionalPropertyValuePropertyOffset(int32 PropertySize)
{
	return FindOwnedPropertySlot(EClassCastFlags::OptionalProperty, PropertySize, 1);
}

/* ClassProperty: MetaClass follows ObjectProperty::PropertyClass. GameModeBase's class members name
   the classes they are constrained to. */
int32_t OffsetFinder::FindClassPropertyMetaClassOffset(int32 PropertyClassOffset)
{
	if (PropertyClassOffset == OffsetNotFound)
		return OffsetNotFound;

	const UEClass GameModeBase = ObjectArray::FindClassFast("GameModeBase");
	if (!GameModeBase)
		return OffsetNotFound;

	std::vector<std::pair<void*, void*>> Infos;

	for (const auto& [MemberName, MetaClassName] : { std::pair{ "PlayerControllerClass", "PlayerController" }, std::pair{ "DefaultPawnClass", "Pawn" }, std::pair{ "HUDClass", "HUD" } })
	{
		void* const Member = GameModeBase.FindMember(MemberName, EClassCastFlags::ClassProperty).GetAddress();
		void* const MetaClass = ObjectArray::FindClassFast(MetaClassName).GetAddress();

		if (Member && MetaClass)
			Infos.push_back({ Member, MetaClass });
	}

	if (Infos.size() < 2)
		return OffsetNotFound;

	return FindOffset<sizeof(void*)>(Infos, PropertyClassOffset + sizeof(void*), PropertyClassOffset + 0x20);
}

/* InterfaceProperty: InterfaceClass, the first member it adds, a class in every sample. */
int32_t OffsetFinder::FindInterfacePropertyInterfaceClassOffset(int32 PropertySize)
{
	return HoldObjectsOfType(SampleProperties(EClassCastFlags::InterfaceProperty, 8), PropertySize, EClassCastFlags::Class) ? PropertySize : OffsetNotFound;
}

/* MulticastDelegateProperty (inline and sparse): SignatureFunction, the first member it adds. */
int32_t OffsetFinder::FindMulticastDelegatePropertySignatureFunctionOffset(int32 PropertySize)
{
	return HoldObjectsOfType(SampleProperties(EClassCastFlags::MulticastDelegateProperty, 8), PropertySize, EClassCastFlags::Function) ? PropertySize : OffsetNotFound;
}

/* FField::FlagsPrivate follows the name. Reflected native properties are all created with the same
   object flags (RF_Public | RF_Transient | RF_MarkAsNative), so FGuid's members agree there, and
   RF_Public is set. */
int32_t OffsetFinder::FindFFieldFlagsOffset()
{
	const UEStruct Guid = ObjectArray::FindStructFast("Guid");
	if (!Guid || Off::FField::Name == OffsetNotFound)
		return OffsetNotFound;

	const int32 Candidate = Off::FField::Name + Off::InSDK::Name::FNameSize;
	const std::vector<UEProperty> Members = Guid.GetProperties();

	if (Members.empty())
		return OffsetNotFound;

	auto FlagsOf = [Candidate](const UEProperty& Prop) { return *reinterpret_cast<const uint32*>(static_cast<const uint8*>(Prop.GetAddress()) + Candidate); };

	const uint32 First = FlagsOf(Members[0]);
	const bool bAllEqual = std::all_of(Members.begin(), Members.end(), [&](const UEProperty& Prop) { return FlagsOf(Prop) == First; });

	return bAllEqual && (First & static_cast<uint32>(EObjectFlags::Public)) ? Candidate : OffsetNotFound;
}

/* FUObjectItem::Flags (EInternalObjectFlags), which the GC keep sets RootSet in. Native classes are
   registered with RF_MarkAsNative | RF_MarkAsRootSet, which UObjectBase::AddObject turns into the
   Native and RootSet internal flags, so the items of a few native classes carry both there. */
int32 OffsetFinder::FindFUObjectItemFlagsOffset()
{
	constexpr uint32 Native = 1u << 25;
	constexpr uint32 RootSet = 1u << 30;

	int32 NumChecked = 0;

	for (const char* ClassName : { "Object", "Class", "Actor" })
	{
		const UEClass Class = ObjectArray::FindClassFast(ClassName);
		const uint8* const Item = Class ? static_cast<const uint8*>(ObjectArray::GetItemAddress(Class.GetIndex())) : nullptr;

		if (!Item)
			continue;

		const uint32 Flags = *reinterpret_cast<const uint32*>(Item + sizeof(void*));

		if ((Flags & (Native | RootSet)) != (Native | RootSet))
			return OffsetNotFound;

		NumChecked++;
	}

	return NumChecked > 0 ? static_cast<int32>(sizeof(void*)) : OffsetNotFound;
}