#pragma once

#include <string>
#include <vector>

#include "Unreal/ObjectArray.h"
#include "PredefinedMembers.h"


/*
* Disassembles every Blueprint UFunction's Kismet bytecode (UStruct::Script, a raw
* TArray<uint8> at offset 0x60/0x68 -- engine-universal across UE4/UE5) into a flat,
* human-readable instruction listing. Native functions (no Script) are skipped.
*
* This is an offline artifact, generated the same pass as the SDK/idmap/usmap -- it needs
* no injected mod, no live hooking, just the same live-process memory access the rest of
* the generator already uses.
*/
class BytecodeGenerator
{
public:
    static inline PredefinedMemberLookupMapType PredefinedMembers;

    static inline std::string MainFolderName = "Bytecode";
    static inline std::string SubfolderName = "";

    static inline fs::path MainFolder;
    static inline fs::path Subfolder;

private:
    using StreamType = std::ofstream;

    /* One decoded instruction. `Offset` is the opcode byte's position in Script; `Depth` is
       sub-expression nesting (drives indentation). */
    struct FInsn
    {
        int32 Offset = 0;
        uint8 Op = 0;
        int32 Depth = 0;
        std::string Text;
    };

    /* Walks a single UFunction's bytecode, mirroring UStruct::SerializeExpr. */
    struct FWalker
    {
        const uint8* Code = nullptr;
        int32 Num = 0;
        int32 Idx = 0;
        bool bFailed = false;
        std::vector<FInsn>* Out = nullptr;
        const std::string* FuncName = nullptr;

        bool Want(int32 N);

        template<typename T>
        T Read()
        {
            T Value{};
            if (!Want(static_cast<int32>(sizeof(T))))
                return Value;

            memcpy(&Value, Code + Idx, sizeof(T));
            Idx += static_cast<int32>(sizeof(T));
            return Value;
        }

        void* ReadPtr();
        std::string ReadName();
        std::string ReadAsciiString();
        std::string ReadUnicodeString();

        uint8 Expr(int32 Depth);
    };

private:
    static const char* TokenName(uint8 Op);

    /* Guarded full-name of a load-fixed-up UObject-derived pointer baked into bytecode. */
    static std::string ObjName(void* Ptr);
    /* Guarded name of an FField / UEFField pointer baked into bytecode. */
    static std::string FieldName(void* Ptr);

    /* Disassembles one function's Script into Out; empty if native or on decode failure. */
    static void DisassembleFunction(UEFunction Func, std::vector<FInsn>& Out);

    static void WriteFunction(StreamType& File, UEFunction Func);

public:
    static void Generate();

    /* Always empty, there are no predefined members for the bytecode dump */
    static void InitPredefinedMembers() { }
    static void InitPredefinedFunctions() { }
};
