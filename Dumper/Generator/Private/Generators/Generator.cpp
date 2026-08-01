
#include "Generators/Generator.h"
#include "Managers/StructManager.h"
#include "Managers/EnumManager.h"
#include "Managers/MemberManager.h"
#include "Managers/PackageManager.h"

#include "HashStringTable.h"
#include "Utils.h"

#include "Platform.h"
#include "Json/json.hpp"

#include <fstream>

inline void InitSettings()
{
	Settings::InitWeakObjectPtrSettings();
	Settings::InitLargeWorldCoordinateSettings();

	Settings::InitObjectPtrPropertySettings();
	Settings::InitArrayDimSizeSettings();
}


void Generator::InitEngineCore()
{
	/* manual override */
	//ObjectArray::Init(/*GObjects*/, /*Layout = Default*/); // FFixedUObjectArray (UEVersion < UE4.21)
	//ObjectArray::Init(/*GObjects*/, /*ChunkSize*/, /*Layout = Default*/); // FChunkedFixedUObjectArray (UEVersion >= UE4.21)

	//FName::Init(/*bForceGNames = false*/);
	//FName::Init(/*AppendString, FName::EOffsetOverrideType::AppendString*/);
	//FName::Init(/*ToString, FName::EOffsetOverrideType::ToString*/);
	//FName::Init(/*GNames, FName::EOffsetOverrideType::GNames, true/false*/);

	//Off::InSDK::ProcessEvent::InitPE(/*PEIndex*/);

	/* Manual override for auxiliary function-address scanners; comment out the matching InitXxx() call below if used. */
	//Off::InSDK::Name::FNameCtorWcharOffset = 0x????????; Settings::Internal::bHasFNameCtorWchar = true;
	//Off::InSDK::Text::FTextCtorFStringOffset = 0x????????; Settings::Internal::bHasFTextCtor = true;
	//Off::InSDK::Engine::UGameEngineTickOffset = 0x????????; Settings::Internal::bHasGameEngineTick = true;
	//Off::InSDK::Construct::StaticConstructObjectInternalOffset = 0x????????; Settings::Internal::bHasStaticConstructObject = true;

	/* Back4Blood (requires manual GNames override) */
	//InitObjectArrayDecryption([](void* ObjPtr) -> uint8* { return reinterpret_cast<uint8*>(uint64(ObjPtr) ^ 0x8375); });

	/* Multiversus [Unsupported, weird GObjects-struct] */
	//InitObjectArrayDecryption([](void* ObjPtr) -> uint8* { return reinterpret_cast<uint8*>(uint64(ObjPtr) ^ 0x1B5DEAFD6B4068C); });

	ObjectArray::Init();

	Off::InSDK::GMalloc::InitGMalloc();

	CALL_PLATFORM_SPECIFIC_FUNCTION(FName::Init);

	Off::Init();
	PropertySizes::Init();

	CALL_PLATFORM_SPECIFIC_FUNCTION(Off::InSDK::ProcessEvent::InitPE); // Must be at this position, relies on offsets initialized in Off::Init()

	Off::InSDK::World::InitGWorld(); // Must be at this position, relies on offsets initialized in Off::Init()

	Off::InSDK::Text::InitTextOffsets(); // Must be at this position, relies on offsets initialized in Off::InitPE()

	/* Auxiliary function-address scanners (best-effort; SDK emitter omits offset on failure). */
	Off::InSDK::Name::InitFNameCtorWchar();                  // Relies on UFunction::ExecFunction (set by Off::Init)
	Off::InSDK::Text::InitFTextCtorFString();                // Relies on UFunction::ExecFunction (set by Off::Init)
	Off::InSDK::Engine::InitUGameEngineTick();               // Relies on PEIndex (set by InitPE) and UClass::CDO (set by Off::Init)
	Off::InSDK::Construct::InitStaticConstructObjectInternal();
	Off::InSDK::ScriptVM::InitScriptVM();                    // Pure signature scan; no engine deps
	Off::InSDK::ScriptContainers::InitScriptContainers();    // Pure signature scan; no engine deps
	Off::InSDK::WeakObject::InitAllocateSerialNumber();      // Pure signature scan; no engine deps
	Off::InSDK::TypeHash::InitTypeHashFunctions();           // Pure signature scan; no engine deps (SDK GetTypeHash)

	InitSettings();
}

void Generator::InitInternal()
{
	// Initialize PackageManager with all packages, their names, structs, classes enums, functions and dependencies
	PackageManager::Init();

	// Initialize StructManager with all structs and their names
	StructManager::Init();
	
	// Initialize EnumManager with all enums and their names
	EnumManager::Init();
	
	// Initialized all Member-Name collisions
	MemberManager::Init();

	// Post-Initialize PackageManager after StructManager has been initialized. 'PostInit()' handles Cyclic-Dependencies detection
	PackageManager::PostInit();
}

bool Generator::SetupDumperFolder()
{
	try
	{
		std::string FolderName = (Settings::Generator::GameVersion + '-' + Settings::Generator::GameName);

		FileNameHelper::MakeValidFileName(FolderName);

		DumperFolder = fs::path(Settings::Generator::SDKGenerationPath) / FolderName;

		if (fs::exists(DumperFolder))
		{
			fs::path Old = DumperFolder.generic_string() + "_OLD";

			fs::remove_all(Old);

			fs::rename(DumperFolder, Old);
		}

		fs::create_directories(DumperFolder);
	}
	catch (const std::filesystem::filesystem_error& fe)
	{
		std::cerr << "Could not create required folders! Info: \n";
		std::cerr << fe.what() << std::endl;
		return false;
	}

	return true;
}

bool Generator::SetupFolders(std::string& FolderName, fs::path& OutFolder)
{
	fs::path Dummy;
	std::string EmptyName = "";
	return SetupFolders(FolderName, OutFolder, EmptyName, Dummy);
}

bool Generator::SetupFolders(std::string& FolderName, fs::path& OutFolder, std::string& SubfolderName, fs::path& OutSubFolder)
{
	FileNameHelper::MakeValidFileName(FolderName);
	FileNameHelper::MakeValidFileName(SubfolderName);

	try
	{
		OutFolder = DumperFolder / FolderName;
		OutSubFolder = OutFolder / SubfolderName;
				
		if (fs::exists(OutFolder))
		{
			fs::path Old = OutFolder.generic_string() + "_OLD";

			fs::remove_all(Old);

			fs::rename(OutFolder, Old);
		}

		fs::create_directories(OutFolder);

		if (!SubfolderName.empty())
			fs::create_directories(OutSubFolder);
	}
	catch (const std::filesystem::filesystem_error& fe)
	{
		std::cerr << "Could not create required folders! Info: \n";
		std::cerr << fe.what() << std::endl;
		return false;
	}

	return true;
}


void DumpEditorOnlyMetadata(const fs::path& DumperFolder)
{
	if (Off::FField::EditorOnlyMetadata == -1)
		return;

	nlohmann::json MetadataJson;
	MetadataJson["GameName"] = Settings::Generator::GameName;
	MetadataJson["GameVersion"] = Settings::Generator::GameVersion;

	for (UEObject Obj : ObjectArray())
	{
		if (!Obj.IsA(EClassCastFlags::Struct))
			continue;

		UEStruct Struct = Obj.Cast<UEStruct>();

		std::vector<UEProperty> ChildProperties = Struct.GetProperties();
		if (ChildProperties.empty()) // Avoids allocating string for GetCppName() and prevents json from auto-creating empty objects for property-less structs
			continue;

		auto& StructMembers = MetadataJson[Struct.GetCppName()];
		for (UEProperty Prop : ChildProperties)
		{
			auto& Entries = StructMembers[Prop.GetValidName()];

			for (const auto& [Key, Value] : Prop.Cast<UEFField>().GetMetaData())
			{
				if (Key.empty() && Value.empty())
					continue;

				Entries[Key] = Value;
			}
		}
	}

	std::ofstream MetadataFile(DumperFolder / "Metadata.json");
	MetadataFile << MetadataJson.dump(4);
}