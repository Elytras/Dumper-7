#include <fstream>
#include <format>

#include "Generators/BytecodeGenerator.h"
#include "Settings.h"


namespace
{
    // UStruct::Script (TArray<uint8>): Data@+0x60, ArrayNum@+0x68. Engine-universal (UE4/UE5).
    constexpr int32 ScriptOffset = 0x60;

    // Skip/jump offsets in modern UE bytecode are 32-bit (CodeSkipSizeType == uint32).
    using CodeSkipSizeType = uint32;

    /* Decoded EExprToken set (UObject/Script.h). enum : uint8 -- EX_Max(0x100) omitted (overflows). */
    enum EExprToken : uint8
    {
        EX_LocalVariable = 0x00, EX_InstanceVariable = 0x01, EX_DefaultVariable = 0x02,
        EX_Return = 0x04, EX_Jump = 0x06, EX_JumpIfNot = 0x07, EX_Assert = 0x09,
        EX_Nothing = 0x0B, EX_Let = 0x0F, EX_ClassContext = 0x12, EX_MetaCast = 0x13,
        EX_LetBool = 0x14, EX_EndParmValue = 0x15, EX_EndFunctionParms = 0x16, EX_Self = 0x17,
        EX_Skip = 0x18, EX_Context = 0x19, EX_Context_FailSilent = 0x1A, EX_VirtualFunction = 0x1B,
        EX_FinalFunction = 0x1C, EX_IntConst = 0x1D, EX_FloatConst = 0x1E, EX_StringConst = 0x1F,
        EX_ObjectConst = 0x20, EX_NameConst = 0x21, EX_RotationConst = 0x22, EX_VectorConst = 0x23,
        EX_ByteConst = 0x24, EX_IntZero = 0x25, EX_IntOne = 0x26, EX_True = 0x27, EX_False = 0x28,
        EX_TextConst = 0x29, EX_NoObject = 0x2A, EX_TransformConst = 0x2B, EX_IntConstByte = 0x2C,
        EX_NoInterface = 0x2D, EX_DynamicCast = 0x2E, EX_StructConst = 0x2F, EX_EndStructConst = 0x30,
        EX_SetArray = 0x31, EX_EndArray = 0x32, EX_PropertyConst = 0x33, EX_UnicodeStringConst = 0x34,
        EX_Int64Const = 0x35, EX_UInt64Const = 0x36, EX_PrimitiveCast = 0x38, EX_SetSet = 0x39,
        EX_EndSet = 0x3A, EX_SetMap = 0x3B, EX_EndMap = 0x3C, EX_SetConst = 0x3D, EX_EndSetConst = 0x3E,
        EX_MapConst = 0x3F, EX_EndMapConst = 0x40, EX_StructMemberContext = 0x42,
        EX_LetMulticastDelegate = 0x43, EX_LetDelegate = 0x44, EX_LocalVirtualFunction = 0x45,
        EX_LocalFinalFunction = 0x46, EX_LocalOutVariable = 0x48, EX_DeprecatedOp4A = 0x4A,
        EX_InstanceDelegate = 0x4B, EX_PushExecutionFlow = 0x4C, EX_PopExecutionFlow = 0x4D,
        EX_ComputedJump = 0x4E, EX_PopExecutionFlowIfNot = 0x4F, EX_Breakpoint = 0x50,
        EX_InterfaceContext = 0x51, EX_ObjToInterfaceCast = 0x52, EX_EndOfScript = 0x53,
        EX_CrossInterfaceCast = 0x54, EX_InterfaceToObjCast = 0x55, EX_WireTracepoint = 0x5A,
        EX_SkipOffsetConst = 0x5B, EX_AddMulticastDelegate = 0x5C, EX_ClearMulticastDelegate = 0x5D,
        EX_Tracepoint = 0x5E, EX_LetObj = 0x5F, EX_LetWeakObjPtr = 0x60, EX_BindDelegate = 0x61,
        EX_RemoveMulticastDelegate = 0x62, EX_CallMulticastDelegate = 0x63,
        EX_LetValueOnPersistentFrame = 0x64, EX_ArrayConst = 0x65, EX_EndArrayConst = 0x66,
        EX_SoftObjectConst = 0x67, EX_CallMath = 0x68, EX_SwitchValue = 0x69,
        EX_InstrumentationEvent = 0x6A, EX_ArrayGetByRef = 0x6B, EX_ClassSparseDataVariable = 0x6C,
        EX_FieldPathConst = 0x6D,
    };
}

const char* BytecodeGenerator::TokenName(uint8 Op)
{
    switch (Op)
    {
    case EX_LocalVariable: return "EX_LocalVariable";
    case EX_InstanceVariable: return "EX_InstanceVariable";
    case EX_DefaultVariable: return "EX_DefaultVariable";
    case EX_Return: return "EX_Return";
    case EX_Jump: return "EX_Jump";
    case EX_JumpIfNot: return "EX_JumpIfNot";
    case EX_Assert: return "EX_Assert";
    case EX_Nothing: return "EX_Nothing";
    case EX_Let: return "EX_Let";
    case EX_ClassContext: return "EX_ClassContext";
    case EX_MetaCast: return "EX_MetaCast";
    case EX_LetBool: return "EX_LetBool";
    case EX_EndParmValue: return "EX_EndParmValue";
    case EX_EndFunctionParms: return "EX_EndFunctionParms";
    case EX_Self: return "EX_Self";
    case EX_Skip: return "EX_Skip";
    case EX_Context: return "EX_Context";
    case EX_Context_FailSilent: return "EX_Context_FailSilent";
    case EX_VirtualFunction: return "EX_VirtualFunction";
    case EX_FinalFunction: return "EX_FinalFunction";
    case EX_IntConst: return "EX_IntConst";
    case EX_FloatConst: return "EX_FloatConst";
    case EX_StringConst: return "EX_StringConst";
    case EX_ObjectConst: return "EX_ObjectConst";
    case EX_NameConst: return "EX_NameConst";
    case EX_RotationConst: return "EX_RotationConst";
    case EX_VectorConst: return "EX_VectorConst";
    case EX_ByteConst: return "EX_ByteConst";
    case EX_IntZero: return "EX_IntZero";
    case EX_IntOne: return "EX_IntOne";
    case EX_True: return "EX_True";
    case EX_False: return "EX_False";
    case EX_TextConst: return "EX_TextConst";
    case EX_NoObject: return "EX_NoObject";
    case EX_TransformConst: return "EX_TransformConst";
    case EX_IntConstByte: return "EX_IntConstByte";
    case EX_NoInterface: return "EX_NoInterface";
    case EX_DynamicCast: return "EX_DynamicCast";
    case EX_StructConst: return "EX_StructConst";
    case EX_EndStructConst: return "EX_EndStructConst";
    case EX_SetArray: return "EX_SetArray";
    case EX_EndArray: return "EX_EndArray";
    case EX_PropertyConst: return "EX_PropertyConst";
    case EX_UnicodeStringConst: return "EX_UnicodeStringConst";
    case EX_Int64Const: return "EX_Int64Const";
    case EX_UInt64Const: return "EX_UInt64Const";
    case EX_PrimitiveCast: return "EX_PrimitiveCast";
    case EX_SetSet: return "EX_SetSet";
    case EX_EndSet: return "EX_EndSet";
    case EX_SetMap: return "EX_SetMap";
    case EX_EndMap: return "EX_EndMap";
    case EX_SetConst: return "EX_SetConst";
    case EX_EndSetConst: return "EX_EndSetConst";
    case EX_MapConst: return "EX_MapConst";
    case EX_EndMapConst: return "EX_EndMapConst";
    case EX_StructMemberContext: return "EX_StructMemberContext";
    case EX_LetMulticastDelegate: return "EX_LetMulticastDelegate";
    case EX_LetDelegate: return "EX_LetDelegate";
    case EX_LocalVirtualFunction: return "EX_LocalVirtualFunction";
    case EX_LocalFinalFunction: return "EX_LocalFinalFunction";
    case EX_LocalOutVariable: return "EX_LocalOutVariable";
    case EX_DeprecatedOp4A: return "EX_DeprecatedOp4A";
    case EX_InstanceDelegate: return "EX_InstanceDelegate";
    case EX_PushExecutionFlow: return "EX_PushExecutionFlow";
    case EX_PopExecutionFlow: return "EX_PopExecutionFlow";
    case EX_ComputedJump: return "EX_ComputedJump";
    case EX_PopExecutionFlowIfNot: return "EX_PopExecutionFlowIfNot";
    case EX_Breakpoint: return "EX_Breakpoint";
    case EX_InterfaceContext: return "EX_InterfaceContext";
    case EX_ObjToInterfaceCast: return "EX_ObjToInterfaceCast";
    case EX_EndOfScript: return "EX_EndOfScript";
    case EX_CrossInterfaceCast: return "EX_CrossInterfaceCast";
    case EX_InterfaceToObjCast: return "EX_InterfaceToObjCast";
    case EX_WireTracepoint: return "EX_WireTracepoint";
    case EX_SkipOffsetConst: return "EX_SkipOffsetConst";
    case EX_AddMulticastDelegate: return "EX_AddMulticastDelegate";
    case EX_ClearMulticastDelegate: return "EX_ClearMulticastDelegate";
    case EX_Tracepoint: return "EX_Tracepoint";
    case EX_LetObj: return "EX_LetObj";
    case EX_LetWeakObjPtr: return "EX_LetWeakObjPtr";
    case EX_BindDelegate: return "EX_BindDelegate";
    case EX_RemoveMulticastDelegate: return "EX_RemoveMulticastDelegate";
    case EX_CallMulticastDelegate: return "EX_CallMulticastDelegate";
    case EX_LetValueOnPersistentFrame: return "EX_LetValueOnPersistentFrame";
    case EX_ArrayConst: return "EX_ArrayConst";
    case EX_EndArrayConst: return "EX_EndArrayConst";
    case EX_SoftObjectConst: return "EX_SoftObjectConst";
    case EX_CallMath: return "EX_CallMath";
    case EX_SwitchValue: return "EX_SwitchValue";
    case EX_InstrumentationEvent: return "EX_InstrumentationEvent";
    case EX_ArrayGetByRef: return "EX_ArrayGetByRef";
    case EX_ClassSparseDataVariable: return "EX_ClassSparseDataVariable";
    case EX_FieldPathConst: return "EX_FieldPathConst";
    default: return "EX_<unknown>";
    }
}

std::string BytecodeGenerator::ObjName(void* Ptr)
{
    if (!Ptr)
        return "null";

    if (reinterpret_cast<uintptr_t>(Ptr) & 0x7)
        return std::format("0x{:X}", reinterpret_cast<uintptr_t>(Ptr));

    return UEObject(Ptr).GetFullName();
}

/* The guarded deref, split into two functions on purpose: __try/__except cannot live in a
   function that needs C++ unwinding (MSVC C2712), so the SEH frame below holds no objects with
   destructors and the std::string work happens in the callee. */
static void BcGetFieldNameRaw(void* Ptr, void* OutStr)
{
    *static_cast<std::string*>(OutStr) = UEFField(Ptr).GetName();
}

static bool BcSehInvoke(void (*Fn)(void*, void*), void* A, void* B)
{
    __try { Fn(A, B); return true; }
    __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}

std::string BytecodeGenerator::FieldName(void* Ptr)
{
    if (!Ptr)
        return "null";

    if (reinterpret_cast<uintptr_t>(Ptr) & 0x7)
        return std::format("0x{:X}", reinterpret_cast<uintptr_t>(Ptr));

    /* Null and misalignment are cheap to reject, but a DESYNCED walk produces pointers that are
       neither: ReadPtr() lands on the wrong 8 bytes of script and yields a value that is non-null
       and 8-aligned yet points at nothing. Dereferencing it walks into FName::AppendString and
       takes the process down mid-dump (observed: AV reading null+0x288 from EX_ArrayConst).

       A dumper walking bytecode it may have misparsed should degrade to a placeholder, not die, so
       the rest of the SDK still gets written. A fault also means we are almost certainly desynced,
       so the operand is reported as a raw address rather than silently looking like a real name. */
    std::string Name;
    if (!BcSehInvoke(&BcGetFieldNameRaw, Ptr, &Name))
        return std::format("<unreadable 0x{:X}>", reinterpret_cast<uintptr_t>(Ptr));

    return Name;
}

bool BytecodeGenerator::FWalker::Want(int32 N)
{
    if (bFailed)
        return false;

    if (N < 0 || Idx < 0 || Idx + N > Num)
    {
        bFailed = true;
        return false;
    }

    return true;
}

void* BytecodeGenerator::FWalker::ReadPtr()
{
    if (!Want(8))
        return nullptr;

    uint64 Value = 0;
    memcpy(&Value, Code + Idx, 8);
    Idx += 8;
    return reinterpret_cast<void*>(static_cast<uintptr_t>(Value));
}

std::string BytecodeGenerator::FWalker::ReadName()
{
    // FScriptName {Comparison@0, Display@4, Number@8} = 12 bytes; live FName only needs
    // {Comparison, Number} = 8 bytes (works for non-case-preserving FName).
    if (!Want(12))
        return "<name>";

    int32 Comparison = 0, Number = 0;
    memcpy(&Comparison, Code + Idx, 4);
    memcpy(&Number, Code + Idx + 8, 4);
    Idx += 12;

    struct { int32 Comparison; int32 Number; } Raw{ Comparison, Number };
    return FName(&Raw).ToString();
}

std::string BytecodeGenerator::FWalker::ReadAsciiString()
{
    std::string S;
    while (Want(1))
    {
        const uint8 C = Code[Idx++];
        if (!C)
            break;
        S += static_cast<char>(C);
    }
    return S;
}

std::string BytecodeGenerator::FWalker::ReadUnicodeString()
{
    std::string S;
    while (Want(2))
    {
        uint16 C = 0;
        memcpy(&C, Code + Idx, 2);
        Idx += 2;
        if (!C)
            break;
        S += (C < 0x80) ? static_cast<char>(C) : '?';
    }
    return S;
}

// Recursive expression walker -- mirrors UStruct::SerializeExpr. Records one FInsn for the
// opcode (pre-order); inline operands go into its Text, sub-expressions become nested FInsns.
uint8 BytecodeGenerator::FWalker::Expr(int32 Depth)
{
    if (bFailed)
        return EX_EndOfScript;

    if (Depth > 256)
    {
        bFailed = true;
        return EX_EndOfScript;
    }

    const int32 OpOff = Idx;
    if (!Want(1))
        return EX_EndOfScript;

    const uint8 Op = Code[Idx++];

    const size_t InsnIdx = Out->size();
    Out->push_back(FInsn{ OpOff, Op, Depth, TokenName(Op) });
    auto SetText = [&](std::string S) { (*Out)[InsnIdx].Text = std::move(S); };
    auto Parms = [&]() { while (!bFailed && Expr(Depth + 1) != EX_EndFunctionParms) {} };

    switch (Op)
    {
    case EX_PrimitiveCast:
        SetText(std::format("EX_PrimitiveCast CST_{}", static_cast<int>(Read<uint8>())));
        Expr(Depth + 1);
        break;

    case EX_ObjToInterfaceCast: case EX_CrossInterfaceCast: case EX_InterfaceToObjCast:
    case EX_MetaCast: case EX_DynamicCast:
        SetText(std::format("{} -> {}", TokenName(Op), ObjName(ReadPtr())));
        Expr(Depth + 1);
        break;

    case EX_Let:
        SetText(std::format("EX_Let {}", FieldName(ReadPtr())));
        Expr(Depth + 1); Expr(Depth + 1);
        break;
    case EX_LetObj: case EX_LetWeakObjPtr: case EX_LetBool:
    case EX_LetDelegate: case EX_LetMulticastDelegate:
        Expr(Depth + 1); Expr(Depth + 1);
        break;
    case EX_LetValueOnPersistentFrame:
        SetText(std::format("EX_LetValueOnPersistentFrame {}", FieldName(ReadPtr())));
        Expr(Depth + 1);
        break;

    case EX_StructMemberContext:
        SetText(std::format("EX_StructMemberContext {}", FieldName(ReadPtr())));
        Expr(Depth + 1);
        break;

    case EX_LocalVariable: case EX_InstanceVariable: case EX_DefaultVariable:
    case EX_LocalOutVariable: case EX_ClassSparseDataVariable: case EX_PropertyConst:
        SetText(std::format("{} {}", TokenName(Op), FieldName(ReadPtr())));
        break;

    case EX_Jump:
    {
        const CodeSkipSizeType T = Read<CodeSkipSizeType>();
        SetText(std::format("EX_Jump -> 0x{:04X}", T));
        break;
    }
    case EX_JumpIfNot:
    {
        const CodeSkipSizeType T = Read<CodeSkipSizeType>();
        SetText(std::format("EX_JumpIfNot -> 0x{:04X}", T));
        Expr(Depth + 1);
        break;
    }
    case EX_Skip:
        SetText(std::format("EX_Skip (size {})", Read<CodeSkipSizeType>()));
        Expr(Depth + 1);
        break;
    case EX_PushExecutionFlow:
    {
        const CodeSkipSizeType T = Read<CodeSkipSizeType>();
        SetText(std::format("EX_PushExecutionFlow -> 0x{:04X}", T));
        break;
    }
    case EX_ComputedJump: case EX_PopExecutionFlowIfNot: case EX_InterfaceContext:
    case EX_Return: case EX_SoftObjectConst: case EX_FieldPathConst:
        Expr(Depth + 1);
        break;

    case EX_ClassContext: case EX_Context: case EX_Context_FailSilent:
        Expr(Depth + 1);                  // object expr
        Read<CodeSkipSizeType>();         // skip-to offset for null context
        ReadPtr();                        // r-value FField* (may be null)
        Expr(Depth + 1);                  // context expr
        break;

    case EX_CallMath: case EX_LocalFinalFunction: case EX_FinalFunction:
    case EX_CallMulticastDelegate:
        SetText(std::format("{} {}", TokenName(Op), ObjName(ReadPtr())));
        Parms();
        break;

    case EX_VirtualFunction: case EX_LocalVirtualFunction:
        SetText(std::format("{} {}", TokenName(Op), ReadName()));
        Parms();
        break;

    case EX_AddMulticastDelegate: case EX_RemoveMulticastDelegate: case EX_ArrayGetByRef:
        Expr(Depth + 1); Expr(Depth + 1);
        break;
    case EX_ClearMulticastDelegate:
        Expr(Depth + 1);
        break;
    case EX_BindDelegate:
        SetText(std::format("EX_BindDelegate {}", ReadName()));
        Expr(Depth + 1); Expr(Depth + 1);
        break;
    case EX_InstanceDelegate:
        SetText(std::format("EX_InstanceDelegate {}", ReadName()));
        break;

    case EX_Assert:
        SetText(std::format("EX_Assert line {}", Read<uint16>()));
        Read<uint8>();          // debug-mode flag
        Expr(Depth + 1);
        break;

    case EX_SwitchValue:
    {
        const uint16 Cases = Read<uint16>();
        const CodeSkipSizeType End = Read<CodeSkipSizeType>();
        SetText(std::format("EX_SwitchValue {} cases -> {}", Cases, End));
        Expr(Depth + 1);                              // index term
        for (uint16 C = 0; C < Cases && !bFailed; ++C)
        {
            Expr(Depth + 1);                          // case value
            Read<CodeSkipSizeType>();                 // offset to next case
            Expr(Depth + 1);                          // case result
        }
        Expr(Depth + 1);                              // default term
        break;
    }

    case EX_StructConst:
    {
        void* S = ReadPtr();
        const int32 Sz = Read<int32>();
        SetText(std::format("EX_StructConst {} ({} bytes)", ObjName(S), Sz));
        while (!bFailed && Expr(Depth + 1) != EX_EndStructConst) {}
        break;
    }
    case EX_SetArray:
        Expr(Depth + 1);                              // array property expr (modern bytecode)
        while (!bFailed && Expr(Depth + 1) != EX_EndArray) {}
        break;
    case EX_SetSet:
        Expr(Depth + 1); Read<int32>();
        while (!bFailed && Expr(Depth + 1) != EX_EndSet) {}
        break;
    case EX_SetMap:
        Expr(Depth + 1); Read<int32>();
        while (!bFailed && Expr(Depth + 1) != EX_EndMap) {}
        break;
    case EX_ArrayConst:
        SetText(std::format("EX_ArrayConst inner={} count={}", FieldName(ReadPtr()), Read<int32>()));
        while (!bFailed && Expr(Depth + 1) != EX_EndArrayConst) {}
        break;
    case EX_SetConst:
        SetText(std::format("EX_SetConst inner={} count={}", FieldName(ReadPtr()), Read<int32>()));
        while (!bFailed && Expr(Depth + 1) != EX_EndSetConst) {}
        break;
    case EX_MapConst:
    {
        void* K = ReadPtr(); void* V = ReadPtr(); const int32 N = Read<int32>();
        SetText(std::format("EX_MapConst key={} val={} count={}", FieldName(K), FieldName(V), N));
        while (!bFailed && Expr(Depth + 1) != EX_EndMapConst) {}
        break;
    }

    case EX_IntConst:        SetText(std::format("EX_IntConst {}", Read<int32>())); break;
    case EX_Int64Const:      SetText(std::format("EX_Int64Const {}", Read<int64>())); break;
    case EX_UInt64Const:     SetText(std::format("EX_UInt64Const {}", Read<uint64>())); break;
    case EX_SkipOffsetConst: SetText(std::format("EX_SkipOffsetConst {}", Read<CodeSkipSizeType>())); break;
    case EX_FloatConst:      SetText(std::format("EX_FloatConst {}", Read<float>())); break;
    case EX_ByteConst: case EX_IntConstByte:
        SetText(std::format("{} {}", TokenName(Op), static_cast<int>(Read<uint8>()))); break;
    case EX_StringConst:     SetText(std::format("EX_StringConst \"{}\"", ReadAsciiString())); break;
    case EX_UnicodeStringConst: SetText(std::format("EX_UnicodeStringConst \"{}\"", ReadUnicodeString())); break;
    case EX_ObjectConst:     SetText(std::format("EX_ObjectConst {}", ObjName(ReadPtr()))); break;
    case EX_NameConst:       SetText(std::format("EX_NameConst {}", ReadName())); break;

    case EX_RotationConst:
        if (Settings::Internal::bUseLargeWorldCoordinates)
            SetText(std::format("EX_RotationConst ({}, {}, {})", Read<double>(), Read<double>(), Read<double>()));
        else
        {
            const int32 P = Read<int32>(), Y = Read<int32>(), R = Read<int32>();
            SetText(std::format("EX_RotationConst ({}, {}, {})", P, Y, R));
        }
        break;
    case EX_VectorConst:
        if (Settings::Internal::bUseLargeWorldCoordinates)
        {
            const double X = Read<double>(), Y = Read<double>(), Z = Read<double>();
            SetText(std::format("EX_VectorConst ({}, {}, {})", X, Y, Z));
        }
        else
        {
            const float X = Read<float>(), Y = Read<float>(), Z = Read<float>();
            SetText(std::format("EX_VectorConst ({}, {}, {})", X, Y, Z));
        }
        break;
    case EX_TransformConst:
        // Rotation(4) + Translation(3) + Scale(3) scalars.
        for (int N = 0; N < 10; ++N)
        {
            if (Settings::Internal::bUseLargeWorldCoordinates)
                Read<double>();
            else
                Read<float>();
        }
        break;

    case EX_TextConst:
    {
        const int Lit = static_cast<int>(Read<uint8>());   // EBlueprintTextLiteralType
        SetText(std::format("EX_TextConst (literal-type {})", Lit));
        switch (Lit)
        {
        case 0: break;                                          // Empty
        case 1: Expr(Depth + 1); Expr(Depth + 1); Expr(Depth + 1); break;  // LocalizedText (ns,key,src)
        case 2: Expr(Depth + 1); break;                         // InvariantText
        case 3: Expr(Depth + 1); break;                         // LiteralString
        case 4: ReadPtr(); Expr(Depth + 1); Expr(Depth + 1); break;        // StringTableEntry
        default: bFailed = true; break;
        }
        break;
    }

    case EX_InstrumentationEvent:   // editor-only; faithful skip (won't appear in shipping)
        if (Want(1) && Code[Idx] == 3 /*EScriptInstrumentation::InlineEvent*/) Idx += 12;
        Read<uint8>();
        break;

    // No-operand tokens -- just the name (already set).
    case EX_Nothing: case EX_EndOfScript: case EX_EndFunctionParms: case EX_EndStructConst:
    case EX_EndArray: case EX_EndArrayConst: case EX_EndSet: case EX_EndMap:
    case EX_EndSetConst: case EX_EndMapConst: case EX_IntZero: case EX_IntOne:
    case EX_True: case EX_False: case EX_NoObject: case EX_NoInterface: case EX_Self:
    case EX_EndParmValue: case EX_PopExecutionFlow: case EX_DeprecatedOp4A:
    case EX_WireTracepoint: case EX_Tracepoint: case EX_Breakpoint:
        break;

    default:
        // Unknown opcode -> operand size unknown -> can't safely continue.
        SetText(std::format("; unknown opcode 0x{:02X} -- aborting decode", Op));
        bFailed = true;
        break;
    }

    return Op;
}

void BytecodeGenerator::DisassembleFunction(UEFunction Func, std::vector<FInsn>& Out)
{
    auto* Base = reinterpret_cast<uint8*>(Func.GetAddress());
    const uint8* Data = *reinterpret_cast<uint8**>(Base + ScriptOffset);
    const int32 Num = *reinterpret_cast<int32*>(Base + ScriptOffset + 0x8);

    if (!Data || Num <= 0)
        return;   // native function, no bytecode

    const std::string FuncName = Func.GetFullName();

    FWalker Walker;
    Walker.Code = Data; Walker.Num = Num; Walker.Out = &Out; Walker.FuncName = &FuncName;

    int32 Guard = 0;
    while (Walker.Idx < Num && !Walker.bFailed)
    {
        if (Walker.Expr(0) == EX_EndOfScript)
            break;

        if (++Guard > Num + 8)
            break;
    }
}

void BytecodeGenerator::WriteFunction(StreamType& File, UEFunction Func)
{
    std::vector<FInsn> Insns;
    DisassembleFunction(Func, Insns);

    if (Insns.empty())
        return;

    File << "\n// " << Func.GetFullName() << "\n";

    for (const FInsn& Insn : Insns)
    {
        File << std::format("[{:04X}] ", Insn.Offset);
        for (int32 I = 0; I < Insn.Depth; ++I)
            File << "  ";
        File << Insn.Text << "\n";
    }
}

void BytecodeGenerator::Generate()
{
    std::string FileName = (Settings::Generator::GameVersion + '-' + Settings::Generator::GameName + "-Bytecode.txt");
    FileNameHelper::MakeValidFileName(FileName);

    std::ofstream File(MainFolder / FileName);

    File << "/*\n"
            "* Blueprint Kismet bytecode disassembly, generated by Dumper-7.\n"
            "*\n"
            "* One section per non-native UFunction; native (C++) functions have no Script and\n"
            "* are skipped. Format: [byte-offset] <indent-by-nesting-depth> EX_Token operand(s).\n"
            "*/\n";

    for (UEObject Obj : ObjectArray())
    {
        if (!Obj.IsA(EClassCastFlags::Class))
            continue;

        for (UEFunction Func : Obj.Cast<UEClass>().GetFunctions())
            WriteFunction(File, Func);
    }
}
