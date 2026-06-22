#pragma once

#include <string>
#include <vector>
#include <filesystem>

#include "Unreal/UnrealObjects.h"
#include "OffsetFinder/Offsets.h"

namespace fs = std::filesystem;

class ObjectArray
{
private:
	friend struct FChunkedFixedUObjectArray;
	friend struct FFixedUObjectArray;
	friend class ObjectArrayValidator;

	friend bool IsAddressValidGObjects(const uintptr_t, const struct FFixedUObjectArrayLayout&);
	friend bool IsAddressValidGObjects(const uintptr_t, const struct FChunkedFixedUObjectArrayLayout&);

private:
	static inline uint8* GObjects = nullptr;
	static inline uint32 NumElementsPerChunk = 0x10000;
	static inline uint32 SizeOfFUObjectItem = sizeof(void*) + sizeof(int32) + sizeof(int32);
	static inline uint32 FUObjectItemInitialOffset = 0x0;

public:
	static inline std::string DecryptionLambdaStr;

private:
	static inline void*(*ByIndex)(void* ObjectsArray, int32 Index, uint32 FUObjectItemSize, uint32 FUObjectItemOffset, uint32 PerChunk) = nullptr;

	/* Same navigation as ByIndex but returns the ADDRESS of the FUObjectItem's Object slot (not the
	   dereferenced UObject*), so callers can reach sibling FUObjectItem fields - notably Flags @ +0x8.
	   Set alongside ByIndex in each Init path (identical math, no final deref). Used by KeepAllFromGC. */
	static inline void*(*ItemAddrByIndex)(void* ObjectsArray, int32 Index, uint32 FUObjectItemSize, uint32 FUObjectItemOffset, uint32 PerChunk) = nullptr;

	static inline uint8_t* (*DecryptPtr)(void* ObjPtr) = [](void* Ptr) -> uint8* { return static_cast<uint8*>(Ptr); };

private:
	static void InitializeFUObjectItem(uint8_t* FirstItemPtr);

	/* OR EInternalObjectFlags::RootSet onto items in [Begin, End); shared by KeepAllFromGC / KeepNewFromGC. */
	static int32 KeepRangeFromGC(int32 Begin, int32 End);

public:
	static void InitDecryption(uint8_t* (*DecryptionFunction)(void* ObjPtr), const char* DecryptionLambdaAsStr);

	static void Init(bool bScanAllMemory = false, const char* const ModuleName = Settings::General::DefaultModuleName);

	static void Init(int32 GObjectsOffset, const FFixedUObjectArrayLayout& ObjectArrayLayout = FFixedUObjectArrayLayout(), const char* const ModuleName = Settings::General::DefaultModuleName);
	static void Init(int32 GObjectsOffset, int32 ElementsPerChunk, const FChunkedFixedUObjectArrayLayout& ObjectArrayLayout = FChunkedFixedUObjectArrayLayout(), const char* const ModuleName = Settings::General::DefaultModuleName);

	/* Opt-in "cursed" GC-keep: OR EInternalObjectFlags::RootSet (0x40000000) onto live FUObjectItems so the
	   engine's GC never collects them -> fuller dumps (and force-loaded assets survive to the dump). Pure
	   memory walk via ItemAddrByIndex; returns the number of items flagged.
	     KeepAllFromGC()        - full sweep [0, Num()); use for the final pass right before dumping.
	     KeepNewFromGC(MaxBatch) - incremental: roots [cursor, min(cursor+MaxBatch, Num())) and advances a
	         persistent forward cursor. Cheap to call every tick - already-rooted objects are immortal so
	         their slots never move/free, meaning we never need to re-mark them; we only march over freshly
	         spawned objects (a few skipped is fine). MaxBatch<=0 means "to the end". */
	static int32 KeepAllFromGC();
	static int32 KeepNewFromGC(int32 MaxBatch);

	static void DumpObjects(const fs::path& Path, bool bWithPathname = false);
	static void DumpObjectsWithProperties(const fs::path& Path, bool bWithPathname = false);

	static int32 Num();
	static int32 Max();
	static int32 NumChunks();
	static int32 MaxChunks();

	template<typename UEType = UEObject>
	static UEType GetByIndex(int32 Index);

	template<typename UEType = UEObject>
	static UEType FindObject(const std::string& FullName, EClassCastFlags RequiredType = EClassCastFlags::None);

	template<typename UEType = UEObject>
	static UEType FindObjectFast(const std::string& Name, EClassCastFlags RequiredType = EClassCastFlags::None);

	template<typename UEType = UEObject>
	static UEType FindObjectFastInOuter(const std::string& Name, std::string Outer);

	static UEStruct FindStruct(const std::string& FullName);
	static UEStruct FindStructFast(const std::string& Name);

	static UEClass FindClass(const std::string& FullName);
	static UEClass FindClassFast(const std::string& Name);

	class ObjectsIterator
	{
		UEObject CurrentObject;
		int32 CurrentIndex;

	public:
		ObjectsIterator(int32 StartIndex = 0);

		UEObject operator*() const;
		ObjectsIterator& operator++();
		bool operator==(const ObjectsIterator& Other) const;
		bool operator!=(const ObjectsIterator& Other) const;

		int32 GetIndex() const;
	};

	ObjectsIterator begin();
	ObjectsIterator end();

	static inline void* DEBUGGetGObjects()
	{
		return GObjects;
	}
};

#ifndef InitObjectArrayDecryption
#define InitObjectArrayDecryption(DecryptionLambda) ObjectArray::InitDecryption(DecryptionLambda, #DecryptionLambda)
#endif

class AllFieldIterator
{
private:
	ObjectArray::ObjectsIterator ObjectEndIterator;
	ObjectArray::ObjectsIterator CurrentObject;
	std::vector<UEProperty> Fields;
	int PropertyIndex = 0;

public:
	AllFieldIterator()
		: CurrentObject(ObjectArray().begin()), ObjectEndIterator(ObjectArray().end())
	{
		if (!IsCurrentObjectStruct())
			IterateToNextStructWithMembers();
	}

	AllFieldIterator(ObjectArray::ObjectsIterator StartPos)
		: CurrentObject(StartPos), ObjectEndIterator(ObjectArray().end())
	{

	}

public:
	inline AllFieldIterator begin() const
	{
		return AllFieldIterator();
	}
	inline AllFieldIterator end() const
	{
		return AllFieldIterator(ObjectArray().end());
	}

	bool operator!=(const AllFieldIterator& Other) const;

	AllFieldIterator& operator++();
	UEProperty operator*() const;

private:
	inline void IterateToNextStruct();
	inline void IterateToNextStructWithMembers();

private:
	inline bool CurrenStructHasMoreMembers() const
	{
		return (static_cast<size_t>(PropertyIndex) + 1) < Fields.size();
	}

	inline UEStruct GetCurrentStruct()
	{
		return (*CurrentObject).Cast<UEStruct>();
	}

	inline bool IsCurrentObjectStruct()
	{
		return (*CurrentObject).IsA(EClassCastFlags::Struct);
	}

	inline bool IsEndIterator() const
	{
		return CurrentObject == ObjectEndIterator;
	}
};
