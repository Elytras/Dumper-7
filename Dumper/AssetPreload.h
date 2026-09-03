#pragma once
/*
AssetPreload.h — force-load the whole cooked asset tree before the dump, so the generated SDK
contains the Blueprint classes the game had not loaded yet.

This is the mod-side `loadassets` command (ue-mods, SharedLib/game/LoadAssetsCommand.cpp) ported
into the dumper. The reason it lives here at all: the mod DLL is built against a *previous* dump's
SDK, whose GObjects/AppendString/ProcessEvent RVAs are stale the moment the game updates — so
running `loadassets` from the mod after an update needed a dump first, then a rebuild, then a
second dump. Dumper-7 resolves those addresses by scanning, so doing the preload here collapses
that back to a single inject.

Nothing below knows a struct layout. Every call is name-based reflection (FindClassFast /
GetFunction / ProcessEvent) and every parameter offset is read from the UFunction's own property
chain, so the same code works on UE4.27 (DRG) and UE5.6 (RC) with no per-build constants.
FAssetData in particular is never interpreted: GetAllAssets hands back an array of them and each
element is memcpy'd straight back into GetAsset as an opaque blob.

Driven from the game-thread Tick hook in main.cpp — Pump() loads a bounded batch per tick, the
same way the mod does it, because force-loading tens of thousands of packages in one frame is how
you get a hitch long enough for the OS to kill the window.
*/

#include <cstdint>
#include <cstring>
#include <format>
#include <iostream>
#include <string>
#include <unordered_map>
#include <vector>

#include "Unreal/ObjectArray.h"
#include "Unreal/UnrealObjects.h"
#include "Unreal/UnrealTypes.h"

namespace AssetPreload
{
	namespace detail
	{
		/* UE's TArray header, the only layout assumed anywhere in this file. It is fixed across every
		   engine version we target, and we treat the elements themselves as opaque bytes. */
		struct FScriptArray
		{
			void* Data;
			int32 Num;
			int32 Max;
		};

		/*
		A parameter buffer for one UFunction, sized and addressed entirely from reflection.

		ParmsSize is the function's PropertiesSize (GetStructSize), and each parameter is found by
		its script name — which is exactly how a Blueprint calls it, and why this survives an engine
		bump that reorders or repads the parms.
		*/
		struct FParms
		{
			UEFunction Fn;
			std::vector<uint8_t> Buf;

			bool Init(UEFunction InFn)
			{
				Fn = InFn;
				if (!Fn)
					return false;
				Buf.assign(static_cast<size_t>(Fn.GetStructSize()), 0);
				return true;
			}

			UEProperty Find(const char* Name) const
			{
				for (UEProperty P : Fn.GetProperties())
				{
					if (P.GetName() == Name)
						return P;
				}
				return UEProperty();
			}

			uint8_t* At(const char* Name)
			{
				UEProperty P = Find(Name);
				return P ? Buf.data() + P.GetOffset() : nullptr;
			}

			uint8_t* Return()
			{
				UEProperty P = Fn.GetReturnProperty();
				return P ? Buf.data() + P.GetOffset() : nullptr;
			}

			/* BP bool parameters are whole bytes (a bitfield bool is a struct-member thing, not a
			   parm thing), so a plain byte write is correct here. */
			void SetBool(const char* Name, bool Value)
			{
				if (uint8_t* P = At(Name))
					*P = Value ? 1 : 0;
			}

			void* Raw() { return Buf.data(); }
		};

		/* ---- resolved once, on the first Pump() ---- */
		inline UEClass g_Helpers;          // AssetRegistryHelpers — carries the static GetAsset/GetAssetRegistry
		inline UEObject g_Registry;        // the IAssetRegistry implementation object
		inline UEFunction g_GetAsset;
		inline FScriptArray g_Assets{};    // engine-owned FAssetData buffer, held across ticks
		inline int32 g_ElemSize = 0;       // sizeof(FAssetData), from its own ScriptStruct
		inline int32 g_ClassNameOff = -1;  // byte offset of the asset-class FName inside FAssetData (-1 = unknown)

		inline int32 g_Index = 0;
		inline int32 g_Ok = 0;
		inline int32 g_Fail = 0;
		inline int32 g_Skipped = 0;
		inline int32 g_EditorOnly = 0;
		inline bool g_Failed = false;      // setup failed — never retry, just let the dump proceed
		inline bool g_Done = false;        // finished — KeepObjectsAlive keeps ticking us afterwards

		/*
		The AssetRegistry singleton, via UAssetRegistryHelpers::GetAssetRegistry().

		It returns a TScriptInterface — {UObject* Object; void* Interface;} — and only the object
		half is useful, since the interface's UFunctions get invoked on it through ProcessEvent.
		*/
		inline UEObject ResolveRegistry(UEClass Helpers)
		{
			UEFunction Fn = Helpers.GetFunction("AssetRegistryHelpers", "GetAssetRegistry");
			if (!Fn)
				return UEObject();

			FParms P;
			if (!P.Init(Fn))
				return UEObject();

			Helpers.ProcessEvent(Fn, P.Raw());

			uint8_t* Ret = P.Return();
			return Ret ? UEObject(*reinterpret_cast<void**>(Ret)) : UEObject();
		}

		/*
		Where the asset's class name lives inside FAssetData, resolved off the struct's own property
		chain. UE4.27 has `AssetClass` (an FName); UE5 replaced it with `AssetClassPath`, an
		FTopLevelAssetPath whose `AssetName` member is the equivalent FName. Both end up as a byte
		offset to an FName, which is all the filters below need.

		Returns -1 if neither shape is found, which just disables the filtering.
		*/
		inline int32 ResolveClassNameOffset(UEStruct AssetData)
		{
			/* Two passes, because UE5 keeps BOTH: AssetClassPath is the live one and AssetClass is a
			   deprecated leftover that is still declared, still reflected, and always None. Taking
			   whichever came first in the property chain picked the dead one and read every asset's
			   class as "None" - which the resident-class filter then rejected, all 46091 of them. */
			UEProperty Legacy;

			for (UEProperty P : AssetData.GetProperties())
			{
				const std::string Name = P.GetName();

				if (Name == "AssetClass")
				{
					Legacy = P;
					continue;
				}

				if (Name == "AssetClassPath")
				{
					UEStruct Path = ObjectArray::FindStructFast("TopLevelAssetPath");
					if (!Path)
						break;

					for (UEProperty Inner : Path.GetProperties())
					{
						if (Inner.GetName() == "AssetName")
							return P.GetOffset() + Inner.GetOffset();
					}
					break;
				}
			}

			return Legacy ? Legacy.GetOffset() : -1;
		}

		inline std::string ClassOf(const uint8_t* Elem)
		{
			if (g_ClassNameOff < 0)
				return std::string();
			return FName(Elem + g_ClassNameOff).ToString();
		}

		/*
		Editor-only assets (EditorUtilityWidget and friends) are listed in the shipped registry but
		their UClass was stripped from the cooked build, so nothing can ever construct them. Testing
		whether the class is resident skips them up front instead of burying the real failures under
		thousands of entries that were never loadable.

		Cached because FindClassFast is a GObjects scan and there are only a few dozen distinct asset
		classes across the whole tree.
		*/
		inline bool ClassResident(const std::string& Cls)
		{
			static std::unordered_map<std::string, bool> Cache;

			auto It = Cache.find(Cls);
			if (It != Cache.end())
				return It->second;

			const bool bOk = static_cast<bool>(ObjectArray::FindClassFast(Cls));
			Cache.emplace(Cls, bOk);
			return bOk;
		}

		/* One-time setup: registry object, asset list, and the two layout facts read from reflection. */
		inline bool Begin()
		{
			g_Helpers = ObjectArray::FindClassFast("AssetRegistryHelpers");
			if (!g_Helpers)
			{
				std::cerr << "[Dumper-7] PreloadAssets: AssetRegistryHelpers not found - skipping preload.\n";
				return false;
			}

			g_GetAsset = g_Helpers.GetFunction("AssetRegistryHelpers", "GetAsset");
			g_Registry = ResolveRegistry(g_Helpers);
			if (!g_GetAsset || !g_Registry)
			{
				/* Split, because one message for two causes cost a dump: the class-name argument was the
				   package name (AssetRegistry) rather than the class (AssetRegistryHelpers), and
				   "unavailable" read as "this game has no registry" instead of "the lookup is wrong". */
				std::cerr << (!g_GetAsset
					? "[Dumper-7] PreloadAssets: AssetRegistryHelpers::GetAsset not found - skipping preload.\n"
					: "[Dumper-7] PreloadAssets: GetAssetRegistry() returned no registry - skipping preload.\n");
				return false;
			}

			UEStruct AssetData = ObjectArray::FindStructFast("AssetData");
			if (!AssetData)
			{
				std::cerr << "[Dumper-7] PreloadAssets: FAssetData struct not found - skipping preload.\n";
				return false;
			}
			g_ElemSize = AssetData.GetStructSize();
			g_ClassNameOff = ResolveClassNameOffset(AssetData);

			/*
			SearchAllAssets and GetAllAssets are INTERFACE methods. They are not on the implementing
			object's SuperStruct chain (its chain is AssetRegistryImpl -> Object), so looking them up
			on the object's own class always misses. Taking them off the interface UClass — whose name
			IS "AssetRegistry", so the lookup matches by construction — and invoking them on the
			implementation is exactly what Blueprint does.
			*/
			UEClass Iface = ObjectArray::FindClassFast("AssetRegistry");
			UEFunction FnSearch = Iface ? Iface.GetFunction("AssetRegistry", "SearchAllAssets") : UEFunction();
			UEFunction FnGetAll = Iface ? Iface.GetFunction("AssetRegistry", "GetAllAssets") : UEFunction();
			if (!FnSearch || !FnGetAll)
			{
				std::cerr << "[Dumper-7] PreloadAssets: AssetRegistry interface reflection not found - skipping preload.\n";
				return false;
			}

			std::cerr << "[Dumper-7] PreloadAssets: scanning AssetRegistry (brief pause)...\n";
			{
				FParms P;
				P.Init(FnSearch);
				P.SetBool("bSynchronousSearch", true);
				g_Registry.ProcessEvent(FnSearch, P.Raw());
			}

			{
				FParms P;
				P.Init(FnGetAll);
				P.SetBool("bIncludeOnlyOnDiskAssets", false);
				g_Registry.ProcessEvent(FnGetAll, P.Raw());

				uint8_t* Out = P.At("OutAssetData");
				if (!Out)
				{
					std::cerr << "[Dumper-7] PreloadAssets: GetAllAssets has no OutAssetData parm - skipping preload.\n";
					return false;
				}
				/* The engine allocated this buffer and nothing here frees it; the process is about to
				   dump and exit, and freeing it with the wrong allocator is a far worse trade. */
				std::memcpy(&g_Assets, Out, sizeof(g_Assets));
			}

			if (!g_Assets.Data || g_Assets.Num <= 0)
			{
				std::cerr << "[Dumper-7] PreloadAssets: AssetRegistry returned no assets - skipping preload.\n";
				return false;
			}

			/*
			A filter that rejects EVERY asset is a broken filter, not a game with nothing to load - and it
			fails silently, because skipping is a legitimate outcome for any single asset. So probe the
			resolved offset against a sample before trusting it: if not one name in the sample names a
			resident UClass, drop the filter and load unfiltered rather than skip the whole tree. Costs a
			few hundred cached FindClassFast calls, once.
			*/
			if (g_ClassNameOff >= 0)
			{
				const int32 Sample = g_Assets.Num < 256 ? g_Assets.Num : 256;
				int32 Resident = 0;

				for (int32 i = 0; i < Sample; i++)
				{
					const std::string Cls = ClassOf(static_cast<const uint8_t*>(g_Assets.Data) + static_cast<size_t>(i) * g_ElemSize);
					if (!Cls.empty() && ClassResident(Cls))
						Resident++;
				}

				if (Resident == 0)
				{
					std::cerr << std::format("[Dumper-7] PreloadAssets: class name at +0x{:X} named no resident class in {} sampled"
						" asset(s) - filter disabled, loading unfiltered.\n", g_ClassNameOff, Sample);
					g_ClassNameOff = -1;
				}
			}

			std::cerr << std::format("[Dumper-7] PreloadAssets: loading {} asset(s)...\n", g_Assets.Num);
			return true;
		}
	}

	/*
	Load up to Budget assets. Returns true while work remains, so the Tick hook can keep calling it
	one batch per frame and only fall through to the dump once it returns false.

	Deliberately does not root anything it loads: KeepObjectsAlive already pins every class (+ its
	package and CDO) each tick, which is the narrow, crash-tested version of keeping a dump's worth
	of objects alive. Rooting the assets themselves is what used to leave whole level packages
	resident and break the engine's later async loads.
	*/
	inline bool Pump(int32 Budget, bool bSkipWorlds = true)
	{
		using namespace detail;

		if (g_Failed || g_Done)
			return false;

		if (!g_Assets.Data)
		{
			if (!Begin())
			{
				g_Failed = true;
				return false;
			}
		}

		detail::FParms P;
		if (!P.Init(g_GetAsset))
		{
			g_Failed = true;
			return false;
		}

		uint8_t* const InAsset = P.At("InAssetData");
		uint8_t* const Ret = P.Return();
		if (!InAsset || !Ret)
		{
			std::cerr << "[Dumper-7] PreloadAssets: GetAsset parms not found - aborting preload.\n";
			g_Failed = true;
			return false;
		}

		int32 Processed = 0;
		for (; g_Index < g_Assets.Num && Processed < Budget; ++g_Index, ++Processed)
		{
			const uint8_t* const Elem = static_cast<const uint8_t*>(g_Assets.Data) + static_cast<size_t>(g_Index) * g_ElemSize;

			const std::string Cls = ClassOf(Elem);
			if (!Cls.empty())
			{
				/* Worlds spawn actors and start ambient audio the moment they load — the game is still
				   running underneath us, so leave them alone unless asked. */
				if (bSkipWorlds && Cls == "World")
				{
					++g_Skipped;
					continue;
				}
				if (!ClassResident(Cls))
				{
					++g_EditorOnly;
					continue;
				}
			}

			/* FAssetData goes in as opaque bytes and comes back out untouched — we never need to know
			   what is inside it, which is what keeps this version-independent. */
			std::memcpy(InAsset, Elem, static_cast<size_t>(g_ElemSize));
			*reinterpret_cast<void**>(Ret) = nullptr;

			g_Helpers.ProcessEvent(g_GetAsset, P.Raw());

			if (*reinterpret_cast<void**>(Ret))
				++g_Ok;
			else
				++g_Fail;
		}

		if (g_Index < g_Assets.Num)
		{
			if ((g_Index % 2000) < Budget)
				std::cerr << std::format("[Dumper-7] PreloadAssets: {}/{} ({} ok, {} failed)\n", g_Index, g_Assets.Num, g_Ok, g_Fail);
			return true;
		}

		/* Latched: with KeepObjectsAlive the hook keeps calling us every tick while it waits for the
		   hotkey, and an unlatched summary printed once per frame until then. */
		g_Done = true;
		std::cerr << std::format("[Dumper-7] PreloadAssets: done - {} loaded, {} failed, {} world(s) skipped, {} editor-only of {}.\n",
			g_Ok, g_Fail, g_Skipped, g_EditorOnly, g_Assets.Num);
		return false;
	}
}
