#pragma once

#include "Unreal/UnrealTypes.h"

class FNameEntry
{
private:
	friend class NameArray;

private:
	static constexpr int32 NameWideMask = 0x1;

private:
	static inline int32 FNameEntryLengthShiftCount = 0x0;

	static inline std::wstring(*GetStr)(uint8* NameEntry) = nullptr;

private:
	uint8* Address;

public:
	FNameEntry() = default;

	FNameEntry(void* Ptr);

public:
	std::wstring GetWString();
	std::string GetString();
	void* GetAddress();

private:
	//Optional to avoid code duplication for FNamePool
	static void Init(const uint8_t* FirstChunkPtr = nullptr, int64 NameEntryStringOffset = 0x0);
};

class NameArray
{
private:
	static inline uint32 FNameBlockOffsetBits = 0x10;

private:
	static uint8* GNames;

	static inline int64 NameEntryStride = 0x0;

	static inline void* (*ByIndex)(void* NamesArray, int32 ComparisonIndex, int32 NamePoolBlockOffsetBits) = nullptr;

	/* Set once the table's extent is known and checked against every object name (see PostInit). */
	static inline bool bBoundsKnown = false;

private:
	static bool InitializeNameArray(uint8_t* NameArray);
	static bool InitializeNamePool(uint8_t* NamePool);

public:
	/* Should be changed later and combined */
	static bool TryFindNameArray_Windows();
	static bool TryFindNamePool_Windows();

	static bool TryInit(bool bIsTestOnly = false);
	static bool TryInit(int32 OffsetOverride, bool bIsNamePool, const char* const ModuleName = Settings::General::DefaultModuleName);

	/* Finds the GNames offset without committing to read name strings from it (FName::AppendString does
	   that). The table is still initialized when possible, for its bounds only - see IsValidCompIdx(). */
	static bool SetGNamesWithoutCommitting();

	static void PostInit();

	/* Whether a comparison index names an entry inside the name table. FName::ToRawWString() refuses an
	   index that doesn't, which is what a name read at a wrong offset usually holds, instead of handing
	   it to the engine's AppendString to follow into unmapped memory. True while the bounds are unknown. */
	static bool IsValidCompIdx(int32 CompIdx);

public:
	static int32 GetNumChunks();

	static int32 GetNumElements();
	static int32 GetByteCursor();

	static FNameEntry GetNameEntry(const void* Name);
	static FNameEntry GetNameEntry(int32 Idx);
};
