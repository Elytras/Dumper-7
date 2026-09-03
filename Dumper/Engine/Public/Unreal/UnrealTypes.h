#pragma once

#include <array>
#include <string>
#include <string_view>
#include <iostream>
#include <Windows.h>

#include "Unreal/Enums.h"
#include "OffsetFinder/Offsets.h"

#include "Encoding/UnicodeNames.h"

#include "Utils.h"
#include "UnrealContainers.h"

using namespace UC;

extern std::string MakeNameValid(std::wstring&& Name);

/*
Fields of a UserDefinedStruct (a Blueprint struct) are named "<DevName>_<Index>_<32-hex GUID>" -
the suffix is editor bookkeeping that keeps a field identifiable across renames, and it is dead
weight in a C++ header. Returns the length of the leading "<DevName>", or npos when the name does
not have that exact shape (so anything else is left completely alone).

Only the emitted C++ identifier is shortened. The raw FName is what the serialization-facing
generators (.usmap, IDA mappings) write, and what reflected by-name lookups use, so neither can
be affected by this. Two fields that collapse to the same DevName are disambiguated by
CollisionManager like any other duplicate member name.
*/
constexpr size_t FindBlueprintFieldSuffix(std::string_view Name)
{
	constexpr size_t GuidLen = 32;

	const size_t GuidUnderscore = Name.find_last_of('_');
	if (GuidUnderscore == std::string_view::npos || GuidUnderscore == 0 || Name.size() - GuidUnderscore - 1 != GuidLen)
		return std::string_view::npos;

	for (size_t i = GuidUnderscore + 1; i < Name.size(); i++)
	{
		const char C = Name[i];
		if (!((C >= '0' && C <= '9') || (C >= 'A' && C <= 'F') || (C >= 'a' && C <= 'f')))
			return std::string_view::npos;
	}

	const size_t IndexUnderscore = Name.find_last_of('_', GuidUnderscore - 1);

	/* No index part, or an empty one, or nothing left in front of it - not the pattern. */
	if (IndexUnderscore == std::string_view::npos || IndexUnderscore == 0 || IndexUnderscore + 1 == GuidUnderscore)
		return std::string_view::npos;

	for (size_t i = IndexUnderscore + 1; i < GuidUnderscore; i++)
	{
		if (Name[i] < '0' || Name[i] > '9')
			return std::string_view::npos;
	}

	return IndexUnderscore;
}

static_assert(FindBlueprintFieldSuffix("isWeakPoint_7_D6DC710948567B685C260D9339A30EA2") == 11, "real BP field must be cut");
static_assert(FindBlueprintFieldSuffix("CategoryName_18_E4EFEE444CAD5B0B31FBCD9DE1D63C5E") == 12, "multi-digit index must be cut");
static_assert(FindBlueprintFieldSuffix("Health") == std::string_view::npos, "plain name untouched");
static_assert(FindBlueprintFieldSuffix("Some_Underscored_Name") == std::string_view::npos, "underscores alone are not the pattern");
static_assert(FindBlueprintFieldSuffix("Foo_7_D6DC710948567B685C260D9339A30EAZ") == std::string_view::npos, "non-hex tail is not a GUID");
static_assert(FindBlueprintFieldSuffix("Foo_7_D6DC710948567B685C260D9339A30EA") == std::string_view::npos, "31 hex digits is not a GUID");
static_assert(FindBlueprintFieldSuffix("Foo_x_D6DC710948567B685C260D9339A30EA2") == std::string_view::npos, "non-numeric index is not the pattern");
static_assert(FindBlueprintFieldSuffix("_7_D6DC710948567B685C260D9339A30EA2") == std::string_view::npos, "never strip a name down to nothing");

template<typename Type>
struct TImplementedInterface
{
	Type InterfaceClass;
	int32 PointerOffset;
	bool bImplementedByK2;
};

using FImplementedInterface = TImplementedInterface<class UEClass>;

class FFreableString : public FString
{
public:
	using FString::FString;

	FFreableString(uint32_t NumElementsToReserve)
	{
		if (NumElementsToReserve > 0x1000000)
			return;

		this->Data = static_cast<wchar_t*>(malloc(sizeof(wchar_t) * NumElementsToReserve));
		this->NumElements = 0;
		this->MaxElements = NumElementsToReserve;
	}

	~FFreableString()
	{
		/* If we're using FName::ToString the allocation comes from the engine and we can not free it. Just leak those 2048 bytes. */
		if (Off::InSDK::Name::bIsUsingAppendStringOverToString)
			FreeArray();
	}

public:
	inline void ResetNum()
	{
		this->NumElements = 0;
	}

private:
	inline void FreeArray()
	{
		this->NumElements = 0;
		this->MaxElements = 0;
		if (this->Data) free(this->Data);
		this->Data = nullptr;
	}
};

class FName
{
public:
	enum class EOffsetOverrideType
	{
		AppendString,
		ToString,
		GNames
	};

private:
	// Ughhh i know this looks ugly but i have no idea how to make this look better
#if defined(_WIN64)
	inline static void(*AppendString)(const void*, FString&) = nullptr;
#elif defined(_WIN32)
	inline static void(__thiscall* AppendString)(const void*, FString&) = nullptr;
#endif

	// Fallback when AppendString was inlined as a combination of 'FNameEntry* FName::GetNameEntry()' and 'void FNameEntry::GetPlainNameString(FString& OutStr)'.
	inline static const void* (*GetNameEntryFromName)(uint32 ComparisonIndex) = nullptr;

	inline static std::wstring(*ToStr)(const void* Name) = nullptr;

private:
	const uint8* Address;

public:
	FName() = default;

	FName(const void* Ptr);

public:
	static void Init_Windows(bool bForceGNames = false);
	static void InitFallback();

	static void Init(int32 OverrideOffset, EOffsetOverrideType OverrideType = EOffsetOverrideType::AppendString, bool bIsNamePool = false, const char* const ModuleName = Settings::General::DefaultModuleName);

private:
	static void* TryFindApendStringBackupStringRef_Windows();

public:
	inline const void* GetAddress() const { return Address; }

	std::wstring ToWString() const;
	std::wstring ToRawWString() const;

	std::string ToString() const;
	std::string ToRawString() const;
	std::string ToValidString() const;

	int32 GetCompIdx() const;
	uint32 GetNumber() const;

	bool operator==(FName Other) const;

	bool operator!=(FName Other) const;

	static std::string CompIdxToString(int CmpIdx);

	static void* DEBUGGetAppendString();
};
