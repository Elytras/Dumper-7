#include "Managers/EnumManager.h"

namespace BlueprintEnumNames
{
	/*
	A UserDefinedEnum (Blueprint enum) does not store the names the dev typed. Its members are literally
	called "NewEnumeratorN"; the real names live in UUserDefinedEnum::DisplayNameMap, a TMap<FName, FText>
	that IS cooked into shipping builds (it is a plain UPROPERTY, not WITH_EDITORONLY_DATA).

	Rather than walk that map and crack FText - both of which are engine-version-specific layouts - ask the
	engine. UKismetNodeHelperLibrary::GetEnumeratorUserFriendlyName(Enum, Value) does the DisplayNameMap
	lookup and the FText->FString conversion internally, and exists in every version we care about
	(4.27 GetDisplayNameTextByValue, 5.6 GetValueOrBitfieldAsDisplayNameText - both keyed by VALUE, not
	index, which matters because BP enum values are not in declaration order). Calling a reflected
	UFunction during a dump is already established practice here (see main.cpp's GetGameName and the
	Conv_StringToText call in Offsets.cpp).

	Everything is best-effort: if the class, the function, or any of its params can't be resolved, this
	returns empty and the caller keeps the original NewEnumeratorN name.
	*/
	static UEFunction GetFriendlyNameFunc()
	{
		static bool bTriedResolve = false;
		static UEFunction Func = nullptr;

		if (!bTriedResolve)
		{
			bTriedResolve = true;

			if (UEClass NodeHelper = ObjectArray::FindClassFast("KismetNodeHelperLibrary"))
				Func = NodeHelper.GetFunction("KismetNodeHelperLibrary", "GetEnumeratorUserFriendlyName");

			if (!Func)
				std::cerr << "Dumper-7: no GetEnumeratorUserFriendlyName, Blueprint enums keep their NewEnumeratorN names.\n";
		}

		return Func;
	}

	/* Returns the dev-facing name for one enumerator, or an empty string if it can't be had. */
	static std::wstring GetUserFriendlyName(UEObject Enum, int64 Value)
	{
		UEFunction Func = GetFriendlyNameFunc();

		/* uint8 param - a value that doesn't fit could never round-trip, so don't even ask. */
		if (!Func || !Enum || Value < 0 || Value > 0xFF)
			return {};

		const UEProperty EnumParam = Func.FindMember("Enum", EClassCastFlags::ObjectProperty);
		const UEProperty ValueParam = Func.FindMember("EnumeratorValue", EClassCastFlags::ByteProperty);
		const UEProperty ReturnParam = Func.FindMember("ReturnValue", EClassCastFlags::StrProperty);

		if (!EnumParam || !ValueParam || !ReturnParam)
			return {};

		const int32 ParamSize = Func.GetStructSize();

#pragma warning(disable: 6255)
		uint8* ParamPtr = static_cast<uint8*>(alloca(ParamSize));
		memset(ParamPtr, 0, ParamSize);

		*reinterpret_cast<void**>(ParamPtr + EnumParam.GetOffset()) = Enum.GetAddress();
		*reinterpret_cast<uint8*>(ParamPtr + ValueParam.GetOffset()) = static_cast<uint8>(Value);

		/* Static function - the object we call it on is irrelevant, same as the Conv_StringToText call. */
		ObjectArray::GetByIndex(0).ProcessEvent(Func, ParamPtr);

		/* The returned FString is engine-allocated, so it is deliberately not freed - freeing an FMemory
		   allocation with our CRT would corrupt the heap. A few thousand short strings per dump. */
		const FString& Return = *reinterpret_cast<FString*>(ParamPtr + ReturnParam.GetOffset());

		return Return.IsValid() ? Return.ToWString() : std::wstring{};
	}
}

namespace EnumInitHelper
{
	template<typename T>
	constexpr inline uint64 GetMaxOfType()
	{
		return (1ull << (sizeof(T) * 0x8ull)) - 1;
	}

	void SetEnumSizeForValue(uint8& Size, uint64 EnumValue)
	{
		if (EnumValue > GetMaxOfType<uint32>()) {
			Size = 0x8;
		}
		else if (EnumValue > GetMaxOfType<uint16>()) {
			Size = max(Size, 0x4);
		}
		else if (EnumValue > GetMaxOfType<uint8>()) {
			Size = max(Size, 0x2);
		}
		else {
			Size = max(Size, 0x1);
		}
	}
}

std::string EnumCollisionInfo::GetUniqueName() const
{
	const std::string Name = EnumManager::GetValueName(*this).GetName();

	if (CollisionCount > 0)
		return Name + "_" + std::to_string(CollisionCount - 1);

	return Name;
}

std::string EnumCollisionInfo::GetRawName() const
{
	return EnumManager::GetValueName(*this).GetName();
}

uint64 EnumCollisionInfo::GetValue() const
{
	return MemberValue;
}

uint8 EnumCollisionInfo::GetCollisionCount() const
{
	return CollisionCount;
}

EnumInfoHandle::EnumInfoHandle(const EnumInfo& InInfo)
	: Info(&InInfo)
{
}

uint8 EnumInfoHandle::GetUnderlyingTypeSize() const
{
	return Info->UnderlyingTypeSize;
}

bool EnumInfoHandle::IsUnderlyingTypeSigned() const
{
	return Info->bIsSigned;
}

const StringEntry& EnumInfoHandle::GetName() const
{
	return EnumManager::GetEnumName(*Info);
}

int32 EnumInfoHandle::GetNumMembers() const
{
	return static_cast<int32>(Info->MemberInfos.size());
}

CollisionInfoIterator EnumInfoHandle::GetMemberCollisionInfoIterator() const
{
	return CollisionInfoIterator(Info->MemberInfos);
}

void EnumManager::InitInternal()
{
	for (auto Obj : ObjectArray())
	{
		if (Obj.HasAnyFlags(EObjectFlags::ClassDefaultObject))
			continue;

		if (!Settings::Internal::bHasUnderlayingTypeInUEnum && Obj.IsA(EClassCastFlags::Struct))
		{
			UEStruct ObjAsStruct = Obj.Cast<UEStruct>();

			for (UEProperty Property : ObjAsStruct.GetProperties())
			{
				if (!Property.IsA(EClassCastFlags::EnumProperty) && !Property.IsA(EClassCastFlags::ByteProperty))
					continue;

				UEEnum Enum = nullptr;
				UEProperty UnderlayingProperty = nullptr;

				if (Property.IsA(EClassCastFlags::EnumProperty))
				{
					Enum = Property.Cast<UEEnumProperty>().GetEnum();
					UnderlayingProperty = Property.Cast<UEEnumProperty>().GetUnderlayingProperty();

					if (!UnderlayingProperty)
						continue;
				}
				else /* ByteProperty */
				{
					Enum = Property.Cast<UEByteProperty>().GetEnum();
					UnderlayingProperty = Property;
				}

				if (!Enum)
					continue;

				EnumInfo& Info = EnumInfoOverrides[Enum.GetIndex()];

				Info.bWasInstanceFound = true;
				Info.UnderlyingTypeSize = 0x1;

				/* Check if the size of this enums underlaying type is greater than the default size (0x1) */
				if (Enum)
				{
					const int32 PropertySize = Property.GetSize();
					Info.UnderlyingTypeSize = max(Info.UnderlyingTypeSize, PropertySize);
					continue;
				}

				if (UnderlayingProperty)
				{
					const int32 PropertySize = UnderlayingProperty.GetSize();
					Info.UnderlyingTypeSize = max(Info.UnderlyingTypeSize, PropertySize);
					continue;
				}
			}
		}
		else if (Obj.IsA(EClassCastFlags::Enum))
		{
			UEEnum ObjAsEnum = Obj.Cast<UEEnum>();

			/* Add name to override info */
			EnumInfo& NewOrExistingInfo = EnumInfoOverrides[Obj.GetIndex()];
			NewOrExistingInfo.Name = UniqueEnumNameTable.FindOrAdd(ObjAsEnum.GetEnumPrefixedName()).first;

			uint64 EnumMaxValue = 0x0;

			/* Initialize enum-member names and their collision infos */
			std::vector<std::pair<FName, int64>> NameValuePairs = ObjAsEnum.GetNameValuePairs();
			for (int i = 0; i < NameValuePairs.size(); i++)
			{
				auto& [Name, Value] = NameValuePairs[i];
					
				std::wstring NameWitPrefix = Name.ToWString();

				/* A negative member forces a signed underlying type so the value can be represented */
				if (Value < 0)
					NewOrExistingInfo.bIsSigned = true;

				if (!NameWitPrefix.ends_with(L"_MAX"))
					EnumMaxValue = max(EnumMaxValue, static_cast<uint64>(Value));

				std::wstring MemberName = NameWitPrefix.substr(NameWitPrefix.find_last_of(L"::") + 1);

				/* Only ever touch the engine's placeholder names - a native enum's member name is already
				   the real one, and must not be replaced by a DisplayName that may differ from it. */
				if (MemberName.starts_with(L"NewEnumerator"))
				{
					std::wstring FriendlyName = BlueprintEnumNames::GetUserFriendlyName(Obj, Value);

					/* Keep the placeholder if the lookup came back empty - a named-but-blank enumerator
					   would collide with every other blank one and read worse than NewEnumeratorN. */
					if (!FriendlyName.empty())
						MemberName = std::move(FriendlyName);
				}

				auto [NameIndex, bWasInserted] = UniqueEnumValueNames.FindOrAdd(MakeNameValid(std::move(MemberName)));

				EnumCollisionInfo CurrentEnumValueInfo;
				CurrentEnumValueInfo.MemberName = NameIndex;
				CurrentEnumValueInfo.MemberValue = Value;

				if (bWasInserted) [[likely]]
				{
					NewOrExistingInfo.MemberInfos.push_back(CurrentEnumValueInfo);
					continue;
				}

				/* A value with this name exists globally, now check if it also exists localy (aka. is duplicated) */
				for (int j = 0; j < i; j++)
				{
					EnumCollisionInfo& CrosscheckedInfo = NewOrExistingInfo.MemberInfos[j];

					if (CrosscheckedInfo.MemberName != NameIndex) [[likely]]
						continue;

					/* Duplicate was found */
					CurrentEnumValueInfo.CollisionCount = CrosscheckedInfo.CollisionCount + 1;
					break;
				}

				/* Check if this name is illegal */
				for (HashStringTableIndex IllegalIndex : IllegalNames)
				{
					if (NameIndex == IllegalIndex) [[unlikely]]
					{
						CurrentEnumValueInfo.CollisionCount++;
						break;
					}
				}

				NewOrExistingInfo.MemberInfos.push_back(CurrentEnumValueInfo);
			}

			if (Settings::Internal::bHasUnderlayingTypeInUEnum)
			{
				auto [Size, bIsSigned] = ObjAsEnum.GetSizeSignedPair();
				NewOrExistingInfo.UnderlyingTypeSize = Size;
				NewOrExistingInfo.bIsSigned = bIsSigned;
			}

			/* Initialize the size based on the highest value contained by this enum */
			if (!NewOrExistingInfo.bWasEnumSizeInitialized && !NewOrExistingInfo.bWasInstanceFound)
			{
				EnumInitHelper::SetEnumSizeForValue(NewOrExistingInfo.UnderlyingTypeSize, EnumMaxValue);
				NewOrExistingInfo.bWasEnumSizeInitialized = true;
			}
		}
	}
}

void EnumManager::InitIllegalNames()
{
	IllegalNames.push_back(UniqueEnumValueNames.FindOrAdd("IN").first);
	IllegalNames.push_back(UniqueEnumValueNames.FindOrAdd("OUT").first);
	IllegalNames.push_back(UniqueEnumValueNames.FindOrAdd("TRUE").first);
	IllegalNames.push_back(UniqueEnumValueNames.FindOrAdd("FALSE").first);
	IllegalNames.push_back(UniqueEnumValueNames.FindOrAdd("DELETE").first);
	IllegalNames.push_back(UniqueEnumValueNames.FindOrAdd("PF_MAX").first);
	IllegalNames.push_back(UniqueEnumValueNames.FindOrAdd("SW_MAX").first);
	IllegalNames.push_back(UniqueEnumValueNames.FindOrAdd("MM_MAX").first);
	IllegalNames.push_back(UniqueEnumValueNames.FindOrAdd("SIZE_MAX").first);
	IllegalNames.push_back(UniqueEnumValueNames.FindOrAdd("RELATIVE").first);
	IllegalNames.push_back(UniqueEnumValueNames.FindOrAdd("TRANSPARENT").first);
	IllegalNames.push_back(UniqueEnumValueNames.FindOrAdd("NO_ERROR").first);
	IllegalNames.push_back(UniqueEnumValueNames.FindOrAdd("EVENT_MAX").first);
	IllegalNames.push_back(UniqueEnumValueNames.FindOrAdd("IGNORE").first);
	IllegalNames.push_back(UniqueEnumValueNames.FindOrAdd("small").first);

	IllegalNames.push_back(UniqueEnumValueNames.FindOrAdd("short").first);
	IllegalNames.push_back(UniqueEnumValueNames.FindOrAdd("long").first);
	IllegalNames.push_back(UniqueEnumValueNames.FindOrAdd("int").first);
	IllegalNames.push_back(UniqueEnumValueNames.FindOrAdd("signed").first);
	IllegalNames.push_back(UniqueEnumValueNames.FindOrAdd("unsigned").first);
}

void EnumManager::Init()
{
	if (bIsInitialized)
		return;

	bIsInitialized = true;

	EnumInfoOverrides.reserve(0x1000);

	InitIllegalNames(); // call this first
	InitInternal();
}
