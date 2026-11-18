#pragma once

#include <filesystem>


/*
* SDK.unity: the whole dump in one file, for a tool that reads it all (ue-mods' genueapi.py) and would otherwise open
* thousands of headers several times each. Written last, beside the dump's SDK folder:
*
*   //@file <size> <path under the dump folder>\n<size bytes of that file>
*
* for every file under SDK/ and the object dump, then a last `//@file <size> @game` entry: JSON of what the game's
* Blueprints hold that the headers do not, read off the live objects (see UnityGenerator.cpp). Without PreloadAssets
* that section only covers what happened to be loaded.
*/
namespace UnityGenerator
{
	void Generate(const std::filesystem::path& DumperFolder);
}
