#include <format>
#include <fstream>
#include <iostream>
#include <unordered_map>
#include <unordered_set>

#include "Generators/UnityGenerator.h"
#include "Generators/BytecodeGenerator.h"
#include "Unreal/ObjectArray.h"
#include "OffsetFinder/Offsets.h"
#include "Json/json.hpp"


/*
* The game section: what genueapi.py's scan_game (and its game_structs / game_enum_types) read off the cooked
* packages of an extract of the same build, read here off the objects those packages loaded into (PreloadAssets). Each
* rule below names the cooked fact it stands for. Paths are spelled as the extract's reader spells them
* (invariants.Package.path): package, then '.', then ':' before every name deeper than the asset.
*
* Where memory and the cooked package can part: an object's cooked export order is not kept (the cook's seek-free sort
* walks serialization, and a cooked game deletes each linker once its package is loaded), so genueapi orders the
* subobject lists itself; and a tag the cook wrote only for a value unlike its archetype's is guessed from the values.
*/
namespace
{
	using json = nlohmann::ordered_json;

	constexpr uint32 RF_ClassDefaultObject = 0x10, RF_ArchetypeObject = 0x20, RF_Transient = 0x40;
	constexpr uint32 RF_DefaultSubObject = 0x40000, RF_InheritableComponentTemplate = 0x400000;
	constexpr uint64 CPF_Deprecated = 0x20000000, CPF_SkipSerialization = 0x0080000000000000;

	uint32 Flags(UEObject Obj) { return static_cast<uint32>(Obj.GetFlags()); }

	/* Names here are FName's raw string (GetNameWithPath, ToRawString): Dumper-7's GetName / ToString cut a package
	   name to its last path segment, "/Game/A/B" to "B". */

	/* invariants.Package.path: "/Game/A/B.B_C", "/Game/A/B.B_C:Func", "/Game/A/B.Default__B_C:Mesh:Sub". */
	std::string PyPath(UEObject Obj)
	{
		if (!Obj)
			return "None";
		UEObject Outer = Obj.GetOuter();
		if (!Outer)
			return Obj.GetNameWithPath();

		std::string Up = PyPath(Outer);
		return Up + (Up.find('.') != std::string::npos ? ':' : '.') + Obj.GetNameWithPath();
	}

	/* UClass members past CastFlags (Class.h): ClassFlags sits right before ClassCastFlags and ClassWithin right after
	   it; ClassConfigName follows, after ClassGeneratedBy where the build keeps that editor-only member (DRG's does:
	   +0x18) - ClassMembersCheck finds which. */
	int32 ConfigAt = 0x10;
	uint32 ClassFlags(UEClass C) { return *reinterpret_cast<uint32*>(reinterpret_cast<uint8*>(C.GetAddress()) + Off::UClass::CastFlags - 4); }
	UEClass ClassWithin(UEClass C) { return UEClass(*reinterpret_cast<void**>(reinterpret_cast<uint8*>(C.GetAddress()) + Off::UClass::CastFlags + 8)); }
	std::string ClassConfig(UEClass C) { return FName(reinterpret_cast<uint8*>(C.GetAddress()) + Off::UClass::CastFlags + ConfigAt).ToRawString(); }

	struct FScriptArray { uint8* Data; int32 Num; int32 Max; };

	/* FUObjectItem::Flags, the EInternalObjectFlags beside the object pointer (UObjectArray.h): PendingKill (4.27's
	   1 << 29, 5.x's Garbage 1 << 21) is there, which the cook does not export. */
	int32 ItemFlags(UEObject Obj)
	{
		auto* Item = reinterpret_cast<uint8*>(ObjectArray::GetItemAddress(Obj.GetIndex()));
		return Item ? *reinterpret_cast<int32*>(Item + sizeof(void*)) : 0;
	}

	/* The game section's builder: the live objects of the game's own .uasset packages (/Game, no map). */
	struct FGame
	{
		std::unordered_map<void*, std::vector<UEObject>> Children;    // outer -> its objects, in GObjects order
		std::unordered_set<void*> Worlds;                              // packages holding a World: maps, which the extract's reader skips
		json Out = json::object();

		bool InGame(UEObject Obj) const
		{
			UEObject Pkg = Obj.GetOutermost();
			return Pkg && !Worlds.contains(Pkg.GetAddress()) && Pkg.GetNameWithPath().starts_with("/Game/");
		}

		const std::vector<UEObject>& Under(UEObject Obj)
		{
			static const std::vector<UEObject> None;
			auto It = Children.find(Obj.GetAddress());
			return It == Children.end() ? None : It->second;
		}

		/* Every object below Root at any depth, with its name path under Root ("Damage:BreakIceBonus_0"). */
		template<typename F>
		void Walk(UEObject Root, const std::string& Prefix, F&& Visit)
		{
			for (UEObject Sub : Under(Root))
			{
				const std::string Path = Prefix.empty() ? Sub.GetNameWithPath() : Prefix + ":" + Sub.GetNameWithPath();
				Visit(Sub, Path);
				Walk(Sub, Path, Visit);
			}
		}

		/* Adds one to Refs[V] for every object V that an ObjectProperty of From (or an element of a TArray of them) names,
		   From itself left out. */
		static void CountRefs(UEObject From, std::unordered_map<void*, int32>& Refs)
		{
			uint8* Base = reinterpret_cast<uint8*>(From.GetAddress());
			for (UEStruct S = From.GetClass(); S; S = S.GetSuper())
			{
				for (UEProperty P : S.GetProperties())
				{
					const std::string Kind = P.GetPropClassName();
					auto Count = [&](void* V) { if (V && V != From.GetAddress()) ++Refs[V]; };
					if (Kind == "ObjectProperty")
						for (int32 I = 0; I < P.GetArrayDim(); ++I)
							Count(*reinterpret_cast<void**>(Base + P.GetOffset() + I * P.GetSize()));
					else if (Kind == "ArrayProperty")
					{
						UEProperty Inner = P.Cast<UEArrayProperty>().GetInnerProperty();
						if (!Inner || Inner.GetPropClassName() != "ObjectProperty")
							continue;
						auto* A = reinterpret_cast<FScriptArray*>(Base + P.GetOffset());
						for (int32 I = 0; A->Data && I < A->Num; ++I)
							Count(reinterpret_cast<void**>(A->Data)[I]);
					}
				}
			}
		}

		/* scan_game's enum_kinds: the native enum each EnumProperty / ByteProperty (and container element) holds. */
		void EnumKinds(UEProperty P)
		{
			if (!P)
				return;
			const std::string Kind = P.GetPropClassName();
			UEEnum E = Kind == "EnumProperty" ? P.Cast<UEEnumProperty>().GetEnum()
				: Kind == "ByteProperty" ? P.Cast<UEByteProperty>().GetEnum() : UEEnum(nullptr);
			if (E)
			{
				const std::string Path = PyPath(E);
				if (Path.starts_with("/Script/"))
				{
					json& Kinds = Out["enum_props"][Path];
					if (std::find(Kinds.begin(), Kinds.end(), Kind) == Kinds.end())
						Kinds.push_back(Kind);
				}
			}
			if (Kind == "ArrayProperty") EnumKinds(P.Cast<UEArrayProperty>().GetInnerProperty());
			else if (Kind == "SetProperty") EnumKinds(P.Cast<UESetProperty>().GetElementProperty());
			else if (Kind == "MapProperty") { EnumKinds(P.Cast<UEMapProperty>().GetKeyProperty()); EnumKinds(P.Cast<UEMapProperty>().GetValueProperty()); }
			else if (Kind == "EnumProperty") EnumKinds(P.Cast<UEEnumProperty>().GetUnderlayingProperty());
		}

		/* A game Blueprint class (scan_game's "GeneratedClass" export). */
		void BlueprintClass(UEClass C)
		{
			const std::string Path = PyPath(C);
			UEClass Within = ClassWithin(C);
			UEStruct Super = C.GetSuper();
			json Interfaces = json::array();
			for (const auto& I : C.GetImplementedInterfaces())
				if (I.InterfaceClass)
					Interfaces.push_back(PyPath(I.InterfaceClass));
			Out["classes"][Path] = { ClassFlags(C), Within ? PyPath(Within) : "/Script/CoreUObject.Object", ClassConfig(C),
			                         Super ? PyPath(Super) : "", Interfaces };

			/* Every default subobject or archetype under the CDO, at any depth. Beside each, its object flags, how many
			   object references inside the CDO's subtree name it, and its internal flags. The cook leaves out one the
			   class's constructor destroyed, which stays in memory flagged PendingKill (4.27) / Garbage (5.x): every FSD AI
			   controller's PathFollowingComponent. genueapi drops those by these flags. */
			UEObject Cdo = C.GetDefaultObject();
			json& Subs = Out["subobjects"][Path] = json::array();
			json& Info = Out["subobject_info"][Path] = json::array();
			if (Cdo)
			{
				std::vector<std::pair<UEObject, std::string>> Tree;
				Walk(Cdo, "", [&](UEObject X, const std::string& NamePath) { Tree.emplace_back(X, NamePath); });
				std::unordered_map<void*, int32> Refs;
				CountRefs(Cdo, Refs);
				for (const auto& [X, NamePath] : Tree)
					CountRefs(X, Refs);
				for (const auto& [X, NamePath] : Tree)
				{
					if (!(Flags(X) & (RF_DefaultSubObject | RF_ArchetypeObject)) || (Flags(X) & RF_Transient))
						continue;
					Subs.push_back(PyPath(X.GetClass()) + " " + NamePath);
					Info.push_back({ Flags(X), Refs[const_cast<void*>(X.GetAddress())], ItemFlags(X) });
				}
			}

			/* The class's own component templates: an ICH record is one flagged RF_InheritableComponentTemplate, and
			   the archetypes instanced under any <Var>_GEN_VARIABLE are its template subobjects. */
			json& Ich = Out["ich"][Path] = json::array();
			json& Templates = Out["templates"][Path] = json::array();
			for (UEObject T : Under(C))
			{
				const std::string Name = T.GetNameWithPath();
				if (!Name.ends_with("_GEN_VARIABLE") || (Flags(T) & RF_Transient))
					continue;
				if (Flags(T) & RF_InheritableComponentTemplate)
					Ich.push_back(Name.substr(0, Name.size() - 13));
				Walk(T, Name, [&](UEObject X, const std::string& NamePath) {
					if ((Flags(X) & RF_ArchetypeObject) && !(Flags(X) & RF_Transient))
						Templates.push_back(PyPath(X.GetClass()) + " " + NamePath);
				});
			}

			for (UEProperty P : C.GetProperties())
				EnumKinds(P);

			if (Super && Cdo && PyPath(Super).starts_with("/Script/"))
				MemberSubobjects(C, Super.Cast<UEClass>(), Cdo);
		}

		/* The CDO's ObjectProperty tags naming one of its own default subobjects, or another CDO's subobject: the
		   cook writes a tag where the value is not its archetype's (the native CDO's), which every member is that
		   the class defines itself. */
		void MemberSubobjects(UEClass C, UEClass Super, UEObject Cdo)
		{
			UEObject SuperCdo = Super.GetDefaultObject();
			const std::string SuperPath = PyPath(Super);
			for (UEStruct S = C; S; S = S.GetSuper())
			{
				for (UEProperty P : S.GetProperties())
				{
					if (P.GetPropClassName() != "ObjectProperty"
					    || (static_cast<uint64>(P.GetPropertyFlags()) & (CPF_Deprecated | CPF_SkipSerialization)))
						continue;
					const bool bInherited = S != C;      // a native member, against the native CDO's value
					for (int32 I = 0; I < P.GetArrayDim(); ++I)
					{
						const int32 At = P.GetOffset() + I * P.GetSize();
						void* V = *reinterpret_cast<void**>(reinterpret_cast<uint8*>(Cdo.GetAddress()) + At);
						if (!V)
							continue;
						if (bInherited && SuperCdo && *reinterpret_cast<void**>(reinterpret_cast<uint8*>(SuperCdo.GetAddress()) + At) == V)
							continue;
						UEObject O(V), Outer = UEObject(V).GetOuter();
						const bool bOwn = Outer == Cdo && (Flags(O) & RF_DefaultSubObject);
						const bool bOther = Outer && Outer != Cdo && Outer.GetNameWithPath().starts_with("Default__")
							&& O.GetOutermost() != Cdo.GetOutermost();
						if (!bOwn && !bOther)
							continue;
						std::string Member = P.GetName();
						std::transform(Member.begin(), Member.end(), Member.begin(), [](char c) { return static_cast<char>(std::tolower(c)); });
						Out["member_subobjects"].push_back({ SuperPath, Member, O.GetNameWithPath() });
					}
				}
			}
		}

		/* A game function (scan_game's FUNCTION_CLASSES export): its owner's function list, the native function it
		   overrides, its properties' enums, and its calls by name on self. */
		void Function(UEFunction F)
		{
			UEObject Owner = F.GetOuter();
			const std::string OwnerPath = PyPath(Owner);
			UEStruct Super = F.GetSuper();
			if (F.GetClass().GetNameWithPath() == "Function" && Super && !(Super.GetOutermost() == F.GetOutermost()))
			{
				const std::string SuperPath = PyPath(Super);
				if (SuperPath.starts_with("/Script/") && SuperPath.find(':') != std::string::npos)
					Out["overrides"].push_back({ OwnerPath, SuperPath.substr(0, SuperPath.find(':')) });
			}
			for (UEProperty P : F.GetProperties())
				EnumKinds(P);
			Out["functions"][OwnerPath].push_back(F.GetNameWithPath());

			std::vector<BytecodeGenerator::FInsn> Insns;
			if (!BytecodeGenerator::DisassembleFunction(F, Insns))
			{
				Out["unread"].push_back(PyPath(F));
				return;
			}
			for (const auto& Insn : Insns)
				if (Insn.Op == 0x1B /* EX_VirtualFunction */ && Insn.bOnSelf)
					Out["self_calls"].push_back({ OwnerPath, Insn.Text.substr(Insn.Text.find(' ') + 1) });
		}

		/* A game object of a native class: each container member's element field class where the object holds
		   one unlike its class default's (the tag the cook writes, which names the element's field class). */
		void NativeObject(UEObject Obj, UEClass C)
		{
			UEObject Cdo = C.GetDefaultObject();
			for (UEStruct S = C; S; S = S.GetSuper())
			{
				for (UEProperty P : S.GetProperties())
				{
					const std::string Kind = P.GetPropClassName();
					std::vector<std::pair<const char*, UEProperty>> Elems;
					if (Kind == "ArrayProperty") Elems = { { "inner", P.Cast<UEArrayProperty>().GetInnerProperty() } };
					else if (Kind == "SetProperty") Elems = { { "inner", P.Cast<UESetProperty>().GetElementProperty() } };
					else if (Kind == "MapProperty") Elems = { { "inner", P.Cast<UEMapProperty>().GetKeyProperty() }, { "value_type", P.Cast<UEMapProperty>().GetValueProperty() } };
					else continue;
					std::erase_if(Elems, [](const auto& E) { if (!E.second) return true; const std::string K = E.second.GetPropClassName(); return K != "EnumProperty" && K != "ByteProperty"; });
					if (Elems.empty())
						continue;
					auto* Mine = reinterpret_cast<FScriptArray*>(reinterpret_cast<uint8*>(Obj.GetAddress()) + P.GetOffset());
					auto* Default = Cdo ? reinterpret_cast<FScriptArray*>(reinterpret_cast<uint8*>(Cdo.GetAddress()) + P.GetOffset()) : nullptr;
					bool bDiffers = Mine->Num != (Default ? Default->Num : 0);
					if (!bDiffers && Mine->Num > 0)
						bDiffers = Kind != "ArrayProperty"
							|| memcmp(Mine->Data, Default->Data, static_cast<size_t>(Mine->Num) * Elems[0].second.GetSize()) != 0;
					if (!bDiffers)
						continue;
					for (const auto& [Slot, E] : Elems)
						Out["containers"].push_back({ PyPath(C), P.GetName(), Slot, E.GetPropClassName() });
				}
			}
		}

		/* game_enum_types' cooked enumerator names: UEnum::Names as the package holds them, `NewEnumerator0`. */
		void UserEnum(UEEnum E)
		{
			json& Names = Out["user_enums"][PyPath(E.GetOutermost())] = json::array();
			for (const auto& [Name, Value] : E.GetNameValuePairs())
			{
				const std::string Full = Name.ToRawString();
				Names.push_back({ Value, Full.substr(Full.rfind(':') == std::string::npos ? 0 : Full.rfind(':') + 1) });
			}
		}

		/* game_structs' default instance tags: per member in property order, its field class and what the cooked tag's
		   value would be where that is knowable ("" = all zero bytes, else hex), and the bool / enum the tag carries. */
		void UserStruct(UEStruct S)
		{
			uint8* Base = reinterpret_cast<uint8*>(S.GetAddress());
			/* Status (ByteProperty) and Guid are UserDefinedStruct's reflected members; DefaultStructInstance, an
			   FStructOnScope {vtable, TWeakObjectPtr ScriptStruct, uint8* SampleStructMemory, ...}, follows Guid. Its
			   weak pointer names the struct itself: that finds it. */
			UEProperty Status = S.GetClass().FindMember("Status"), Guid = S.GetClass().FindMember("Guid");
			if (!Status || !Guid)
				return;
			json Row = json::object();
			Row["status"] = Base[Status.GetOffset()];
			uint8* Sample = nullptr;
			for (int32 At = (Guid.GetOffset() + 0x10 + 7) & ~7; At < Guid.GetOffset() + 0x40 && !Sample; At += 8)
				if (*reinterpret_cast<int32*>(Base + At) == S.GetIndex())
					Sample = *reinterpret_cast<uint8**>(Base + At + 8);
			if (!Sample)
			{
				std::cerr << "[Dumper-7] Unity: no default instance found on " << S.GetPathName() << "\n";
				return;
			}
			json& Tags = Row["defaults"] = json::array();
			for (UEProperty P : S.GetProperties())
			{
				const std::string Kind = P.GetPropClassName();
				uint8* V = Sample + P.GetOffset();
				std::string Value = "01", Enum;
				int Bool = 0;
				auto Hex = [&](int32 N) { std::string H; for (int32 I = 0; I < N; ++I) H += std::format("{:02x}", V[I]); return H; };
				if (Kind == "BoolProperty")
				{
					UEBoolProperty B = P.Cast<UEBoolProperty>();
					Bool = (V[B.GetByteOffset()] & B.GetFieldMask()) != 0;
					Value = "";
				}
				else if (Kind == "ByteProperty")
				{
					UEEnum E = P.Cast<UEByteProperty>().GetEnum();
					Enum = E ? E.GetNameWithPath() : "None";
					Value = E ? "01" : Hex(1);     // an enum's byte is its enumerator's FName in the tag
				}
				else if (Kind == "IntProperty" || Kind == "FloatProperty" || Kind == "Int64Property" || Kind == "DoubleProperty"
				         || Kind == "UInt32Property" || Kind == "Int16Property" || Kind == "UInt16Property" || Kind == "Int8Property"
				         || Kind == "UInt64Property")
					Value = Hex(P.GetSize());
				else if (Kind == "ObjectProperty" || Kind == "ClassProperty" || Kind == "WeakObjectProperty" || Kind == "LazyObjectProperty"
				         || Kind == "InterfaceProperty")
					Value = *reinterpret_cast<void**>(V) ? "01" : "";
				else if (Kind == "StrProperty" || Kind == "ArrayProperty" || Kind == "SetProperty" || Kind == "MapProperty")
					Value = reinterpret_cast<FScriptArray*>(V)->Num ? "01" : "";
				if (Value.find_first_not_of('0') == std::string::npos)
					Value = "";
				Tags.push_back({ P.GetName(), Kind, Value, Bool, Enum });
			}
			Out["user_structs"][PyPath(S.GetOutermost())] = Row;
		}

		void Build()
		{
			for (UEObject Obj : ObjectArray())
			{
				if (UEObject Outer = Obj.GetOuter())
					Children[Outer.GetAddress()].push_back(Obj);
				if (Obj.GetClass() && Obj.GetClass().GetNameWithPath() == "World")
					Worlds.insert(Obj.GetOutermost().GetAddress());
			}
			for (const char* Key : { "classes", "subobjects", "subobject_info", "ich", "templates", "functions", "enum_props", "user_enums", "user_structs" })
				Out[Key] = json::object();
			for (const char* Key : { "member_subobjects", "overrides", "self_calls", "containers", "unread" })
				Out[Key] = json::array();

			for (UEObject Obj : ObjectArray())
			{
				UEClass C = Obj.GetClass();
				if (!C || !InGame(Obj) || (Flags(Obj) & RF_ClassDefaultObject))
					continue;
				const std::string Kind = C.GetNameWithPath();
				if (Kind == "BlueprintGeneratedClass" || Kind == "WidgetBlueprintGeneratedClass" || Kind == "AnimBlueprintGeneratedClass")
					BlueprintClass(Obj.Cast<UEClass>());
				else if (Kind == "Function" || Kind == "DelegateFunction" || Kind == "SparseDelegateFunction")
					Function(Obj.Cast<UEFunction>());
				else if (Kind == "UserDefinedEnum")
					UserEnum(Obj.Cast<UEEnum>());
				else if (Kind == "UserDefinedStruct")
					UserStruct(Obj.Cast<UEStruct>());
				else if (PyPath(C).starts_with("/Script/"))
					NativeObject(Obj, C);
			}
		}
	};

	/* The offsets the class rows read past CastFlags, checked on UObject's own class: within itself, config Engine. */
	bool ClassMembersCheck()
	{
		UEClass Object = ObjectArray::FindClassFast("Object");
		if (!Object || ClassWithin(Object) != Object || !(ClassFlags(Object) & 0x80 /* CLASS_Native */))
			return false;
		for (ConfigAt = 0x10; ConfigAt <= 0x18; ConfigAt += 8)
			if (ClassConfig(Object) == "Engine")
				return true;
		return false;
	}
}

void UnityGenerator::Generate(const std::filesystem::path& DumperFolder)
{
	namespace fs = std::filesystem;

	std::vector<fs::path> Files;
	for (const auto& Entry : fs::recursive_directory_iterator(DumperFolder / "SDK"))
		if (Entry.is_regular_file())
			Files.push_back(Entry.path());
	for (const char* Name : { "GObjects-Dump-WithProperties.txt", "Offsets.txt" })
		if (fs::exists(DumperFolder / Name))
			Files.push_back(DumperFolder / Name);

	/* As SDK.unity.part until whole, so a watcher (ue-mods' tools/vanilla_dump.py) knows the dump is still going. */
	const fs::path Part = DumperFolder / "SDK.unity.part";
	std::ofstream Unity(Part, std::ios::binary);
	auto Finish = [&]() { Unity.close(); fs::rename(Part, DumperFolder / "SDK.unity"); };
	for (const fs::path& File : Files)
	{
		std::ifstream In(File, std::ios::binary);
		const std::string Body((std::istreambuf_iterator<char>(In)), std::istreambuf_iterator<char>());
		Unity << "//@file " << Body.size() << " " << fs::relative(File, DumperFolder).generic_string() << "\n" << Body;
	}

	if (!ClassMembersCheck())
	{
		UEClass Object = ObjectArray::FindClassFast("Object");
		std::cerr << std::format("[Dumper-7] Unity: UClass::ClassWithin / ClassConfigName are not where expected (UObject's"
			" within {}, flags 0x{:X}) - no game section.\n", Object ? PyPath(ClassWithin(Object)) : "?", Object ? ClassFlags(Object) : 0);
		Finish();
		return;
	}
	FGame Game;
	try
	{
		Game.Build();
	}
	catch (const std::exception& E)
	{
		std::cerr << "[Dumper-7] Unity: the game section failed (" << E.what() << ") - none written.\n";
		Finish();
		return;
	}
	const std::string Section = Game.Out.dump(-1, ' ', false, json::error_handler_t::replace);
	Unity << "//@file " << Section.size() << " @game\n" << Section;
	Finish();
	std::cerr << std::format("[Dumper-7] Unity: {} file(s) and the game section in SDK.unity:\n", Files.size());
	for (const auto& [Key, Value] : Game.Out.items())
	{
		std::cerr << std::format("[Dumper-7] Unity:   {} {}", Key, Value.size());
		int32 Shown = 0;
		for (const auto& [Sub, Row] : Value.items())
		{
			if (Shown++ == 2)
				break;
			std::cerr << " | " << (Value.is_object() ? Sub + " = " : "") << Row.dump(-1, ' ', false, json::error_handler_t::replace).substr(0, 160);
		}
		std::cerr << "\n";
	}
	/* The extract of CL 158958 holds 4,569 game Blueprint classes; far fewer is a filter gone wrong, not a smaller game. */
	if (Game.Out["classes"].size() < 1000)
		std::cerr << std::format("[Dumper-7] Unity: WARNING - only {} game Blueprint classes in the game section; "
			"an extract of a DRG build has thousands. Check InGame() and the PreloadAssets result.\n", Game.Out["classes"].size());
}
