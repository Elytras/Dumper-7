#include <format>

#include "Utils.h"

#include "OffsetFinder/Offsets.h"
#include "OffsetFinder/OffsetFinder.h"

#include "Unreal/ObjectArray.h"
#include "Unreal/NameArray.h"

#include "Platform.h"
#include "Architecture.h"


void Off::InSDK::ProcessEvent::InitPE_Windows()
{
#ifdef PLATFORM_WINDOWS

	void** Vft = *(void***)ObjectArray::GetByIndex(0).GetAddress();

#if defined(_WIN64)
	/* Primary, and more reliable, check for ProcessEvent */
	auto IsProcessEvent = [](const uint8_t* FuncAddress, [[maybe_unused]] int32_t Index) -> bool
		{
			return Platform::FindPatternInRange({ 0xF7, -0x1, Off::UFunction::FunctionFlags, 0x0, 0x0, 0x0, 0x0, 0x04, 0x0, 0x0 }, FuncAddress, 0x400)
				&& Platform::FindPatternInRange({ 0xF7, -0x1, Off::UFunction::FunctionFlags, 0x0, 0x0, 0x0, 0x0, 0x0, 0x40, 0x0 }, FuncAddress, 0xF00);
		};
#elif defined(_WIN32)
	/* Primary, and more reliable, check for ProcessEvent */
	auto IsProcessEvent = [](const uint8_t* FuncAddress, [[maybe_unused]] int32_t Index) -> bool
		{
			return Platform::FindPatternInRange({ 0xF7, -0x1, Off::UFunction::FunctionFlags, 0x0, 0x4, 0x0, 0x0 }, FuncAddress, 0x400)
				&& Platform::FindPatternInRange({ 0xF7, -0x1, Off::UFunction::FunctionFlags, 0x0, 0x0, 0x40, 0x0 }, FuncAddress, 0xF00);
		};
#endif

	const void* ProcessEventAddr = nullptr;
	int32_t ProcessEventIdx = 0;

	const auto [FuncPtr, FuncIdx] = Platform::IterateVTableFunctions(Vft, IsProcessEvent);

	ProcessEventAddr = FuncPtr;
	ProcessEventIdx = FuncIdx;

	if (!FuncPtr)
	{
		const void* StringRefAddr = Platform::FindByStringInAllSections(L"Accessed None", 0x0, 0x0, Settings::General::bSearchOnlyExecutableSectionsForStrings);
		/* ProcessEvent is sometimes located right after a func with the string L"Accessed None. Might as well check for it, because else we're going to crash anyways. */
		const void* PossiblePEAddr = reinterpret_cast<void*>(Architecture_x86_64::FindNextFunctionStart(StringRefAddr));

		auto IsSameAddr = [PossiblePEAddr](const uint8_t* FuncAddress, [[maybe_unused]] int32_t Index) -> bool
			{
				return FuncAddress == PossiblePEAddr;
			};

		const auto [FuncPtr2, FuncIdx2] = Platform::IterateVTableFunctions(Vft, IsSameAddr);
		ProcessEventAddr = FuncPtr2;
		ProcessEventIdx = FuncIdx2;
	}

	if (ProcessEventAddr)
	{
		Off::InSDK::ProcessEvent::PEIndex = ProcessEventIdx;
		Off::InSDK::ProcessEvent::PEOffset = static_cast<int32>(Platform::GetOffset(ProcessEventAddr));

		std::cerr << std::format("PE-Offset: 0x{:X}\n", Off::InSDK::ProcessEvent::PEOffset);
		std::cerr << std::format("PE-Index: 0x{:X}\n\n", ProcessEventIdx);
		return;
	}

	std::cerr << "\nCouldn't find ProcessEvent!\n\n" << std::endl;

#endif // PLATFORM_WINDOWS
}

void Off::InSDK::GMalloc::InitGMalloc()
{
#ifdef PLATFORM_WINDOWS
#if defined(_WIN64)

	// CreateGMalloc prologue: sub rsp, N then mov rax, gs:[0x58].
	// UE5.x inserts a `mov ecx, [rip+_tls_index]` (8B 0D ..) between them for thread-safe-static init;
	// UE4.27 has no interlude. The generic UE4 wildcard is broad enough to hit other TLS-using
	// functions, so try the most-specific sigs first and fall back to the wildcard last.
	//   UE4.27 (DRG): sub rsp, 0xA8 ; mov rax, gs:[0x58] ; mov ecx, [rip+tls_index]
	//                The tls_index load follows the gs read (not before it as in UE5).
	//                Exact rsp size + trailing 8B 0D makes this unique (2 matches without it).
	//   UE5.x:        sub rsp, N ; mov ecx, [rip+tls_index] ; mov rax, gs:[0x58]
	//   UE4 generic:  sub rsp, N ; mov rax, gs:[0x58]  (last resort, any rsp size)
	const char* createSig_UE4_DRG     = "48 81 EC A8 00 00 00 65 48 8B 04 25 58 00 00 00 8B 0D ?? ?? ?? ??";
	const char* createSig_UE5         = "48 81 EC ?? ?? ?? ?? 8B 0D ?? ?? ?? ?? 65 48 8B 04 25 58 00 00 00";
	const char* createSig_UE4_generic = "48 81 EC ?? ?? ?? ?? 65 48 8B 04 25 58 00 00 00";

	void* createMatch = Platform::FindPattern(createSig_UE4_DRG, 0x0, false, 0x0, nullptr);
	if (!createMatch)
		createMatch = Platform::FindPattern(createSig_UE5, 0x0, false, 0x0, nullptr);
	if (!createMatch)
		createMatch = Platform::FindPattern(createSig_UE4_generic, 0x0, false, 0x0, nullptr);

	if (!createMatch)
	{
		std::cerr << "Failed to find CreateGMalloc\n";
		return;
	}

	uint8_t* createBase = reinterpret_cast<uint8_t*>(createMatch);
	uint64 base = Platform::GetModuleBase();

	CreateGMallocOffset = static_cast<int32>(createBase - reinterpret_cast<uint8_t*>(base));
	std::cerr << std::format("CreateGMalloc-Offset: 0x{:X}\n", CreateGMallocOffset);

	// Scan CreateGMalloc body for the first RIP-relative reference to a global -> GMalloc.
	// Matches: mov reg, [rip+disp] / mov [rip+disp], reg / cmp [rip+disp], imm8.
	// All three forms put the disp32 at +3 with instruction length 7.
	uint8_t* gmallocRef = nullptr;
	for (int32_t i = 0; i + 7 <= 0x200; ++i)
	{
		const uint8_t b0 = createBase[i + 0];
		const uint8_t b1 = createBase[i + 1];
		const uint8_t b2 = createBase[i + 2];

		const bool bIsMov = (b0 == 0x48) && (b1 == 0x89 || b1 == 0x8B) && ((b2 & 0xC7) == 0x05);
		const bool bIsCmp = (b0 == 0x48) && (b1 == 0x83) && (b2 == 0x3D); // cmp [rip+disp32], imm8

		if (!bIsMov && !bIsCmp)
			continue;

		const int32_t rel = *reinterpret_cast<int32_t*>(createBase + i + 3);
		uint8_t* const target = createBase + i + 7 + rel;

		if (!Platform::IsAddressInProcessRange(reinterpret_cast<uintptr_t>(target)))
			continue;

		gmallocRef = createBase + i;
		GMallocOffset = static_cast<int32>(target - reinterpret_cast<uint8_t*>(base));
		break;
	}

	if (!gmallocRef)
	{
		std::cerr << "Failed to find GMalloc reference inside CreateGMalloc\n";
		return;
	}

	std::cerr << std::format("GMalloc-Offset: 0x{:X}\n", GMallocOffset);

	Settings::Internal::bHasGMalloc = true;

#endif
#endif
}
void Off::InSDK::ProcessEvent::InitPE(const int32 Index, const char* const ModuleName)
{
	Off::InSDK::ProcessEvent::PEIndex = Index;

	void** VFT = *reinterpret_cast<void***>(ObjectArray::GetByIndex(0).GetAddress());

	Off::InSDK::ProcessEvent::PEOffset = static_cast<int32>(Platform::GetOffset(VFT[Off::InSDK::ProcessEvent::PEIndex], ModuleName));

	std::cerr << std::format("PE-Offset: 0x{:X}\n", Off::InSDK::ProcessEvent::PEOffset);
}

/* Native exec address of a Kismet helper UFUNCTION by name. Returns 0 if unavailable. */
static uintptr_t GetKismetExecAddress(const char* const FunctionName)
{
	const UEFunction Fn = ObjectArray::FindObjectFast<UEFunction>(FunctionName, EClassCastFlags::Function);
	if (!Fn)
		return 0x0;

	if (Off::UFunction::ExecFunction == 0x0)
		return 0x0;

	void* const ExecPtr = *reinterpret_cast<void**>(reinterpret_cast<uint8_t*>(const_cast<void*>(Fn.GetAddress())) + Off::UFunction::ExecFunction);
	if (!ExecPtr || !Platform::IsAddressInProcessRange(reinterpret_cast<uintptr_t>(ExecPtr)))
		return 0x0;

	return reinterpret_cast<uintptr_t>(ExecPtr);
}

/*
* FName::FName(const wchar_t*, EFindName) — native string->FName ctor.
*
* Reached from the reflected exec UFunction 'Conv_StringToName' (100% findable by name). In its call
* chain the ctor is invoked with EFindName::FNAME_Add (== 1) as its 3rd argument, so we anchor on the
* 'mov r8d, 1' immediate that precedes the call — semantically invariant, unlike picking by call order
* (the chain also touches a GC/lock helper, FFrame::Step/StepExplicitProperty and FMemory::Free, and
* which branch runs shifts the call indices).
*
* The immediate sits in different functions across engine versions: UE5 / RogueCore inlines the
* reflected wrapper into the exec stub (immediate in the stub itself), while UE4.27 / Deep Rock keeps a
* separate reflected 'Conv_StringToName' that the stub calls (immediate one level down). So we walk the
* call-subtree from the stub (depth-bounded, each body bounded by FindFunctionEnd so a scan can't bleed
* into an adjacent function) instead of scanning only the stub. Decoupled from the FText finder.
*/
static uintptr_t FindFNameCtorViaImmediate(uintptr_t Fn)
{
	if (!Platform::IsAddressInProcessRange(Fn))
		return 0x0;

	const uintptr_t End = Architecture_x86_64::FindFunctionEnd(Fn, 0x400);
	const uintptr_t Range = (End > Fn && (End - Fn) <= 0x400) ? (End - Fn) : 0x150;

	// 41 B8 01 00 00 00 = mov r8d, 1 (EFindName::FNAME_Add). The compiler may hoist it ~0x1C above the
	// call (setnz/add/cmp/mov/cmovnz in between), so DON'T require adjacency — anchor on the immediate,
	// then resolve the first 'call rel32' that follows it.
	void* const Hit = Platform::FindPatternInRange("41 B8 01 00 00 00", Fn, Range);
	if (!Hit)
		return 0x0;

	const uintptr_t Ctor = Architecture_x86_64::GetRipRelativeCalledFunction(reinterpret_cast<uintptr_t>(Hit), 1, nullptr);
	return Platform::IsAddressInProcessRange(Ctor) ? Ctor : 0x0;
}

static uintptr_t FindFNameCtorWcharRec(uintptr_t Fn, int32 Depth)
{
	if (const uintptr_t Ctor = FindFNameCtorViaImmediate(Fn))
		return Ctor;

	if (Depth <= 0)
		return 0x0;

	for (int32 k = 1; k <= 8; ++k)
	{
		const uintptr_t Child = Architecture_x86_64::GetRipRelativeCalledFunction(Fn, k, nullptr);
		if (!Child || Child == Fn)
			continue;

		if (const uintptr_t Found = FindFNameCtorWcharRec(Child, Depth - 1))
			return Found;
	}

	return 0x0;
}

void Off::InSDK::Name::InitFNameCtorWchar()
{
#ifdef PLATFORM_WINDOWS
#if defined(_WIN64)
	const uintptr_t NameExec = GetKismetExecAddress("Conv_StringToName");
	if (!NameExec)
	{
		std::cerr << "FName ctor: Conv_StringToName UFunction not found, skipping.\n";
		return;
	}

	// Depth 2: exec stub (UE5/RC, immediate inlined) -> reflected Conv_StringToName (UE4.27/DRG) -> ctor.
	const uintptr_t Ctor = FindFNameCtorWcharRec(NameExec, 2);
	if (!Ctor)
	{
		std::cerr << "FName ctor: EFindName immediate not found in exec stub or its callees, override manually.\n";
		return;
	}

	Off::InSDK::Name::FNameCtorWcharOffset = static_cast<int32>(Ctor - Platform::GetModuleBase());
	Settings::Internal::bHasFNameCtorWchar = true;
	std::cerr << std::format("FName::FName(const wchar_t*, EFindName) (EFindName anchor): 0x{:X}\n",
		Off::InSDK::Name::FNameCtorWcharOffset);
#endif
#endif
}

/*
* FText-from-FString primitive (the FText(FString&&) / CultureInvariant builder) — native string->FText.
*
* UE5+ has no 'Conv_StringToText' UFunction to anchor on, and the Conv_*ToText helpers use FText::Format
* (format strings), not this. So we reach it from the reflected 'Conv_NameToText' exec, whose call chain
* is: execConv_NameToText -> Conv_NameToText -> (FName::ToString, then) the FString->FText builder
* sub_141257000. We walk that call-subtree (FindFunctionEnd-bounded so a scan can't bleed into an
* adjacent function) and return the first callee that both allocates an FTextHistory_Base (operator
* new(0x38)) and flags its result CultureInvariant (or dword[reg+8], 2). See IsFTextFromStringPrimitive
* for why we match the 0x02 flag (not the FromName-only 0x12). Decoupled from the FName finder.
*/
static bool IsFTextFromStringPrimitive(uintptr_t Fn)
{
	if (!Platform::IsAddressInProcessRange(Fn))
		return false;

	const uintptr_t End = Architecture_x86_64::FindFunctionEnd(Fn, 0x800);
	if (End <= Fn || (End - Fn) > 0x800)
		return false; // can't bound the body safely -> don't risk a false positive

	const uintptr_t Range = End - Fn;

	// FText layout differs by engine, so the CultureInvariant flag-set we anchor on does too:
	//   UE4.27 (DRG): 24-byte FText {FTextData* @0; FSharedRefController* @8; uint32 Flags @0x10}.
	//                 Conv_NameToText -> FText::AsCultureInvariant(FString) sets 'or dword[reg+0x10], 2'
	//                 (= 83 ?? 10 02) and builds via FText::GetEmpty + refcounting (NO FTextHistory_Base
	//                 operator new(0x38)), so the flag-set alone anchors it. Imm 0x02 (CultureInvariant
	//                 only) is the variant Conv_NameToText reaches; the 0x12 (|Transient) variant lives in
	//                 FText::FromString, which Conv_NameToText does not call.
	//   UE5.x:        16-byte intrusive-refcount FText (Flags @ +0x8). The primitive allocates an
	//                 FTextHistory_Base (operator new(0x38) = 'mov ecx, 0x38' = B9 38 00 00 00) and sets
	//                 'or dword[reg+8], 2' (= 83 ?? 08 02). Requiring BOTH keeps the (looser) 0x02 immediate
	//                 from matching the GC/FFrame helpers also reachable from the exec stub.
	// Gate on the already-resolved FText size (>= 0x18 == UE4.27 24-byte shape).
	if (Off::InSDK::Text::TextSize >= 0x18)
		return Platform::FindPatternInRange("83 ?? 10 02", Fn, Range) != nullptr;

	return Platform::FindPatternInRange("83 ?? 08 02", Fn, Range) != nullptr
		&& Platform::FindPatternInRange("B9 38 00 00 00", Fn, Range) != nullptr;
}

static uintptr_t FindFTextFromString(uintptr_t Fn, int32 Depth)
{
	if (!Platform::IsAddressInProcessRange(Fn))
		return 0x0;

	if (IsFTextFromStringPrimitive(Fn))
		return Fn;

	if (Depth <= 0)
		return 0x0;

	for (int32 k = 1; k <= 8; ++k)
	{
		const uintptr_t Child = Architecture_x86_64::GetRipRelativeCalledFunction(Fn, k, nullptr);
		if (!Child || Child == Fn)
			continue;

		const uintptr_t Found = FindFTextFromString(Child, Depth - 1);
		if (Found)
			return Found;
	}

	return 0x0;
}

void Off::InSDK::Text::InitFTextCtorFString()
{
#ifdef PLATFORM_WINDOWS
#if defined(_WIN64)
	const uintptr_t Exec = GetKismetExecAddress("Conv_NameToText");
	if (!Exec)
	{
		std::cerr << "FText::FromString: Conv_NameToText UFunction not found, skipping.\n";
		return;
	}

	// exec -> Conv_NameToText -> FString->FText builder (sub_141257000) — up to 3 hops deep
	const uintptr_t FromString = FindFTextFromString(Exec, 3);
	if (!FromString)
	{
		std::cerr << "FText::FromString: culture-invariant flag-set anchor not found, override manually.\n";
		return;
	}

	Off::InSDK::Text::FTextCtorFStringOffset = static_cast<int32>(FromString - Platform::GetModuleBase());
	Settings::Internal::bHasFTextCtor = true;
	std::cerr << std::format("FText::FromString (CultureInvariant builder): 0x{:X}\n",
		Off::InSDK::Text::FTextCtorFStringOffset);
#endif
#endif
}

/*
* UGameEngine::Tick — anchored off UGameEngine::HandleBrowseToDefaultMapFailure.
*
* HandleBrowseToDefaultMapFailure contains a `lea rN, [rip+disp]` loading the unique UTF-16 literal
* "UGameEngine::HandleBrowseToDefaultMapFailure" (passed to a Request-Exit call). Once we find which
* UGameEngine vtable slot's function contains that LEA, Tick sits at a known fixed offset earlier in
* the override block:
*
*     Tick                            vtable[N - 7 or N - 8]
*     GetMaxTickRate, ProcessToggleFreeze*, NetworkRemapPath, ShouldDoAsyncEndOfFrameTasks
*     [Exec]                          (only when UE_ALLOW_EXEC_COMMANDS == 1)
*     GetGameViewportWidget
*     HandleBrowseToDefaultMapFailure vtable[N]
*
* Window is widened to {-6..-9} to absorb minor UE5.x layout shifts; pick the largest function in that
* window (Tick is ~2-5KB, comfortably larger than Exec).
*/
void Off::InSDK::Engine::InitUGameEngineTick()
{
#ifdef PLATFORM_WINDOWS
#if defined(_WIN64)
	const UEClass GameEngine = ObjectArray::FindClassFast("GameEngine");
	if (!GameEngine)
	{
		std::cerr << "UGameEngine::Tick: GameEngine class not found, skipping.\n";
		return;
	}

	const void* const CDO = *reinterpret_cast<void* const*>(
		reinterpret_cast<const uint8_t*>(GameEngine.GetAddress()) + Off::UClass::ClassDefaultObject);
	if (!CDO || Platform::IsBadReadPtr(CDO))
	{
		std::cerr << "UGameEngine::Tick: GameEngine CDO unreadable, skipping.\n";
		return;
	}

	void** const Vft = *reinterpret_cast<void** const*>(CDO);
	if (!Vft || Platform::IsBadReadPtr(Vft))
	{
		std::cerr << "UGameEngine::Tick: GameEngine vtable unreadable, skipping.\n";
		return;
	}

	// FindByStringInAllSections returns the LEA address, not the string address. Resolve to verify.
	const void* const LeaAddr = Platform::FindByStringInAllSections(
		L"UGameEngine::HandleBrowseToDefaultMapFailure", 0x0, 0x0, /*bSearchOnlyExecutableSections=*/true);
	if (!LeaAddr)
	{
		std::cerr << "UGameEngine::Tick: no LEA referencing 'UGameEngine::HandleBrowseToDefaultMapFailure' found, skipping.\n";
		return;
	}

	const uintptr_t StringTarget = Architecture_x86_64::Resolve32BitRelativeLea(reinterpret_cast<uintptr_t>(LeaAddr));
	if (!Platform::IsAddressInProcessRange(StringTarget))
	{
		std::cerr << "UGameEngine::Tick: resolved string address out of range, skipping.\n";
		return;
	}

	// Find the vtable slot whose function contains the LEA: largest slot start <= LEA, within a sane
	// function-size bound. Avoids FindFunctionEnd, which doesn't terminate on jmp-tail-call functions.
	const uintptr_t LeaAddrUint = reinterpret_cast<uintptr_t>(LeaAddr);
	const int32 StartIdx = Off::InSDK::ProcessEvent::PEIndex + 1;
	const int32 EndIdx = StartIdx + 0x80;

	constexpr uintptr_t MaxPlausibleFnSize = 0x400;

	int32 AnchorIdx = -1;
	uintptr_t BestSlotAddr = 0;
	for (int32 i = StartIdx; i < EndIdx; ++i)
	{
		const uintptr_t SlotAddr = reinterpret_cast<uintptr_t>(Vft[i]);
		if (!SlotAddr || !Platform::IsAddressInProcessRange(SlotAddr))
			break; // End of vtable

		if (SlotAddr > LeaAddrUint)
			continue;
		if (LeaAddrUint - SlotAddr > MaxPlausibleFnSize)
			continue;

		if (SlotAddr > BestSlotAddr)
		{
			BestSlotAddr = SlotAddr;
			AnchorIdx = i;
		}
	}

	if (AnchorIdx == -1)
	{
		std::cerr << "UGameEngine::Tick: LEA address not contained by any UGameEngine vtable slot, skipping.\n";
		return;
	}

	const uint64 base = Platform::GetModuleBase();

	// Tick is the largest function in {-6..-9} from the anchor.
	int32 BestTickIdx = -1;
	uintptr_t BestTickTarget = 0;
	uintptr_t BestTickSize = 0x400;

	for (int32 Delta = 6; Delta <= 9; ++Delta)
	{
		const int32 TickIdx = AnchorIdx - Delta;
		if (TickIdx <= Off::InSDK::ProcessEvent::PEIndex)
			continue;

		const uintptr_t Target = reinterpret_cast<uintptr_t>(Vft[TickIdx]);
		if (!Platform::IsAddressInProcessRange(Target))
			continue;

		const uintptr_t End = Architecture_x86_64::FindFunctionEnd(Target, 0x4000);
		const uintptr_t Size = End ? (End - Target) : 0;

		if (Size > BestTickSize)
		{
			BestTickSize = Size;
			BestTickTarget = Target;
			BestTickIdx = TickIdx;
		}
	}

	if (BestTickIdx != -1)
	{
		Off::InSDK::Engine::UGameEngineTickOffset = static_cast<int32>(BestTickTarget - base);
		Off::InSDK::Engine::UGameEngineTickIndex = BestTickIdx;
		Settings::Internal::bHasGameEngineTick = true;
		std::cerr << std::format(
			"UGameEngine::Tick: 0x{:X} (vtable[{}], HandleBrowse anchor at vtable[{}], size 0x{:X})\n",
			Off::InSDK::Engine::UGameEngineTickOffset, BestTickIdx, AnchorIdx, BestTickSize);
	}
	else
	{
		std::cerr << std::format(
			"UGameEngine::Tick: HandleBrowse at vtable[{}] but no slot in [-9..-6] looks like Tick, override manually.\n",
			AnchorIdx);
	}
#endif
#endif
}

/*
* StaticConstructObject_Internal — multi-pattern scan.
*
* No stable string anchor lives inside this function in shipping builds, so we fall back to prologue
* signatures. MSVC chooses different callee-saved-storage strategies (push vs shadow-space save)
* per build, so four distinct prologue families cover the common UE 4.27 -> UE 5.6 shapes:
*   A) UE 5.6 r11-frame-aliased       (Deadly Days, Tokyo Xtreme, Whiskerwood, Drainsim)
*   B) UE 4.27 DRG-style              (Deep Rock Galactic, Ghost Wire Tokyo)
*   C) UE 5.6 three-shadow-saves      (StarRupture, Vein)
*   D) UE 4.27 Walking Dead-style     (Walking Dead Saints & Sinners)
* Frame sizes and the LEA displacement are wildcarded so each family covers minor build variations.
* False-positive risk is real on busy binaries; the chosen address is logged and a manual override
* is provided in Generator::InitEngineCore() for cases where the auto-pick is wrong.
*/
void Off::InSDK::Construct::InitStaticConstructObjectInternal()
{
#ifdef PLATFORM_WINDOWS
#if defined(_WIN64)
	// A: mov r11, rsp; push rbp/rbx/r14; lea rbp, [r11+disp]; sub rsp, N
	const char* sigA = "4C 8B DC 55 53 41 56 49 8D AB ?? ?? ?? ?? 48 81 EC ?? ?? ?? ??";
	// B: mov [rsp+10/18], rbx/rsi; push rbp/rdi/r12/r14/r15; lea rbp, [rsp+disp]; sub rsp, N
	const char* sigB = "48 89 5C 24 10 48 89 74 24 18 55 57 41 54 41 56 41 57 48 8D AC 24 ?? ?? ?? ?? 48 81 EC ?? ?? ?? ??";
	// C: mov [rsp+10/18/20], rbx/rsi/rdi; push rbp/r12/r13/r14/r15; lea rbp, [rsp+disp]; sub rsp, N
	const char* sigC = "48 89 5C 24 10 48 89 74 24 18 48 89 7C 24 20 55 41 54 41 55 41 56 41 57 48 8D AC 24 ?? ?? ?? ?? 48 81 EC ?? ?? ?? ??";
	// D: mov [rsp+10/18/20], rbx/rbp/rsi; push rdi/r14/r15; sub rsp, N
	const char* sigD = "48 89 5C 24 10 48 89 6C 24 18 48 89 74 24 20 57 41 56 41 57 48 81 EC ?? ?? ?? ??";

	struct Candidate { const char* sig; const char* family; };
	const Candidate candidates[] = { {sigA, "A"}, {sigB, "B"}, {sigC, "C"}, {sigD, "D"} };

	void* match = nullptr;
	const char* matchedFamily = nullptr;
	for (const auto& c : candidates)
	{
		match = Platform::FindPattern(c.sig, 0x0, false, 0x0, nullptr);
		if (match)
		{
			matchedFamily = c.family;
			break;
		}
	}

	if (!match)
	{
		std::cerr << "StaticConstructObject_Internal: no prologue pattern matched, override manually.\n";
		return;
	}

	const uint64 base = Platform::GetModuleBase();
	Off::InSDK::Construct::StaticConstructObjectInternalOffset =
		static_cast<int32>(reinterpret_cast<uint8_t*>(match) - reinterpret_cast<uint8_t*>(base));
	Settings::Internal::bHasStaticConstructObject = true;

	std::cerr << std::format(
		"StaticConstructObject_Internal (family {}): 0x{:X}\n",
		matchedFamily, Off::InSDK::Construct::StaticConstructObjectInternalOffset);
#endif
#endif
}

/*
* Off::InSDK::ScriptVM::InitScriptVM
*
* Blueprint-VM steppers. FFrame::Step is the inner interpreter loop: it reads *Code++ (Code @ FFrame+0x20)
* and tail-dispatches GNatives[op]. Its prologue is byte-identical on UE 4.27 (DRG) and UE 5.6 (RC) — the
* only build variance is the rip-relative displacement of the `lea r9, [rip+GNatives]`, which we wildcard,
* so a single signature covers both engine generations. GNatives (the EX-opcode -> handler dispatch table,
* the backbone of the VM) is then decoded deterministically from that lea. StepExplicitProperty does
* `bt PropertyFlags, 8` (CPF_OutParm); its only version delta is the FProperty PropertyFlags byte offset
* (r8+0x38 on UE5.6, r8+0x40 on UE4.27), which we mask. All addresses logged; manual override available.
*/
void Off::InSDK::ScriptVM::InitScriptVM()
{
#ifdef PLATFORM_WINDOWS
#if defined(_WIN64)
	// mov rax,[rcx+20]; mov r10,rdx; mov rdx,rcx; movzx ecx,[rax]; inc rax; mov [rcx+20],rax;
	// mov eax,r9d; lea r9,[rip+GNatives]; mov rcx,r10; jmp qword [r9+rax*8]
	const char* stepSig =
		"48 8B 41 20 4C 8B D2 48 8B D1 44 0F B6 08 48 FF C0 48 89 41 20 "
		"41 8B C1 4C 8D 0D ?? ?? ?? ?? 49 8B CA 49 FF 24 C1";
	void* const step = Platform::FindPattern(stepSig, 0x0, false, 0x0, nullptr);
	if (!step)
	{
		std::cerr << "ScriptVM: FFrame::Step pattern not matched, skipping (override manually).\n";
		return;
	}

	const uint64 base = Platform::GetModuleBase();
	Off::InSDK::ScriptVM::FFrameStepOffset =
		static_cast<int32>(reinterpret_cast<uint8_t*>(step) - reinterpret_cast<uint8_t*>(base));

	// GNatives: the first `4C 8D 0D <disp32>` (lea r9,[rip+disp32]) inside Step. lea is 7 bytes, rip points
	// at the following instruction, so target = &lea + 7 + disp32.
	void* const lea = Platform::FindPatternInRange("4C 8D 0D", reinterpret_cast<uintptr_t>(step), 0x40);
	if (lea)
	{
		const uint8_t* const p = reinterpret_cast<const uint8_t*>(lea);
		const int32 disp = *reinterpret_cast<const int32*>(p + 3);
		const uintptr_t gnatives = reinterpret_cast<uintptr_t>(p) + 7 + disp;
		if (Platform::IsAddressInProcessRange(gnatives))
			Off::InSDK::ScriptVM::GNativesOffset = static_cast<int32>(gnatives - base);
	}

	// FFrame::StepExplicitProperty: `mov eax,[r8+PropertyFlags]; ...; bt rax,8; jae ...` — mask the
	// PropertyFlags byte (0x38 UE5.6 / 0x40 UE4.27). `48 0F BA E0 08` (bt rax,8) makes it distinctive.
	const char* stepExplicitSig = "41 8B 40 ?? 4D 8B C8 4C 8B D1 48 0F BA E0 08 73";
	void* const stepExplicit = Platform::FindPattern(stepExplicitSig, 0x0, false, 0x0, nullptr);
	if (stepExplicit)
		Off::InSDK::ScriptVM::FFrameStepExplicitPropertyOffset =
			static_cast<int32>(reinterpret_cast<uint8_t*>(stepExplicit) - reinterpret_cast<uint8_t*>(base));

	Settings::Internal::bHasScriptVM = true;
	std::cerr << std::format(
		"ScriptVM: FFrame::Step 0x{:X} | StepExplicitProperty 0x{:X} | GNatives 0x{:X}\n",
		Off::InSDK::ScriptVM::FFrameStepOffset,
		Off::InSDK::ScriptVM::FFrameStepExplicitPropertyOffset,
		Off::InSDK::ScriptVM::GNativesOffset);
#endif
#endif
}

/*
* Off::InSDK::ScriptContainers::InitScriptContainers
*
* Reflected-container (TMap) edit helpers. FScriptMapHelper::FindOrAdd / RemoveAt operate on a stack
* FScriptMapHelper {KeyProp, ValueProp, Map, MapLayout, MapFlags}; their UE5.6 prologue unpacks that
* helper inline (movzx eax,[rcx+0x30]=MapFlags; lea/mov of MapLayout@+0x18, KeyProp@+0, ValueProp@+8,
* Map@+0x10), which is a distinctive, stable fingerprint. The function frame-size immediate is wildcarded.
* Independent of InitScriptVM (a Step-pattern miss must not block these). UE4.27 lacks the MapFlags field,
* so this is a UE5-shaped scan; it simply leaves the offsets at 0 on engines where it doesn't match.
*/
void Off::InSDK::ScriptContainers::InitScriptContainers()
{
#ifdef PLATFORM_WINDOWS
#if defined(_WIN64)
	const uint64 base = Platform::GetModuleBase();

	// FScriptMapHelper::FindOrAdd(this in rcx, KeyPtr in rdx) -> pair index.
	const char* findOrAddSig =
		"40 55 53 57 48 8D 6C 24 ?? 48 81 EC ?? ?? ?? ?? 0F B6 41 30 48 8D 79 18 "
		"4C 8B 01 F6 D0 4C 8B 49 08 48 8B 59 10 A8 01";
	void* const findOrAdd = Platform::FindPattern(findOrAddSig, 0x0, false, 0x0, nullptr);
	if (findOrAdd)
		Off::InSDK::ScriptContainers::FScriptMapHelperFindOrAddOffset =
			static_cast<int32>(reinterpret_cast<uint8_t*>(findOrAdd) - reinterpret_cast<uint8_t*>(base));

	// FScriptMapHelper::RemoveAt(this in rcx, Index in edx).
	const char* removeAtSig =
		"48 89 5C 24 10 48 89 6C 24 18 48 89 74 24 20 41 56 48 83 EC ?? "
		"0F B6 41 30 41 8B E8 48 8B 71 10 F6 D0";
	void* const removeAt = Platform::FindPattern(removeAtSig, 0x0, false, 0x0, nullptr);
	if (removeAt)
		Off::InSDK::ScriptContainers::FScriptMapHelperRemoveAtOffset =
			static_cast<int32>(reinterpret_cast<uint8_t*>(removeAt) - reinterpret_cast<uint8_t*>(base));

	// FScriptMapHelper::FindMapPairIndexFromHash(this in rcx, KeyPtr in rdx) -> int32 pair index.
	const char* findPairSig =
		"4C 8B DC 48 83 EC ?? 0F B6 41 30 4C 8D 41 18 4C 8B 09 F6 D0 4C 8B 51 10 A8 01 "
		"49 8D 43 08 4D 89 4B 08 4D 89 4B 10";
	void* const findPair = Platform::FindPattern(findPairSig, 0x0, false, 0x0, nullptr);
	if (findPair)
		Off::InSDK::ScriptContainers::FScriptMapHelperFindPairIndexOffset =
			static_cast<int32>(reinterpret_cast<uint8_t*>(findPair) - reinterpret_cast<uint8_t*>(base));

	// FScriptSetHelper::RemoveElement(this in rcx, ElementPtr in rdx) -> bool (one-call remove-by-value).
	const char* setRemoveElementSig =
		"4C 8B DC 53 56 41 56 48 83 EC ?? 48 8B 01 4D 8D 4B D8 4C 8B F1 49 89 43 08 49 89 43 18 49 8D 4B 08 49 8D 43 18 49 89 4B";
	void* const setRemoveElement = Platform::FindPattern(setRemoveElementSig, 0x0, false, 0x0, nullptr);
	if (setRemoveElement)
		Off::InSDK::ScriptContainers::FScriptSetHelperRemoveElementOffset =
			static_cast<int32>(reinterpret_cast<uint8_t*>(setRemoveElement) - reinterpret_cast<uint8_t*>(base));

	// FScriptSetHelper::RemoveAt(this in rcx, Index in edx, Count in r8d).
	const char* setRemoveAtSig =
		"48 89 5C 24 10 48 89 6C 24 18 57 48 83 EC ?? 41 8B E8 8B DA 48 8B F9 E8 ?? ?? ?? ?? 85 ED 0F 84 ?? ?? ?? ?? 48 89 74 24";
	void* const setRemoveAt = Platform::FindPattern(setRemoveAtSig, 0x0, false, 0x0, nullptr);
	if (setRemoveAt)
		Off::InSDK::ScriptContainers::FScriptSetHelperRemoveAtOffset =
			static_cast<int32>(reinterpret_cast<uint8_t*>(setRemoveAt) - reinterpret_cast<uint8_t*>(base));

	// FScriptSetHelper::AddElement(this in rcx, ElementPtr in rdx) -> int32 sparse index (one-call insert-by-
	// value; builds its 4 closures then tail-calls core TScriptSet::Add). Fingerprint = the helper unpack:
	// mov r9,[rcx]=ElementProp; lea r8,[rcx+0x10]=&SetLayout; movups xmm0,[r8]; mov rcx,[rcx+8]=Set. Frame-size
	// immediate + security-cookie disp masked.
	const char* setAddElementSig =
		"4C 8B DC 48 81 EC ?? ?? ?? ?? 48 8B 05 ?? ?? ?? ?? 48 33 C4 48 89 84 24 ?? ?? ?? ?? "
		"4C 8B 09 4C 8D 41 10 41 0F 10 00 48 8B 49 08 48 8D 44 24";
	void* const setAddElement = Platform::FindPattern(setAddElementSig, 0x0, false, 0x0, nullptr);
	if (setAddElement)
		Off::InSDK::ScriptContainers::FScriptSetHelperAddElementOffset =
			static_cast<int32>(reinterpret_cast<uint8_t*>(setAddElement) - reinterpret_cast<uint8_t*>(base));

	if (findOrAdd || removeAt || findPair || setRemoveElement || setRemoveAt || setAddElement)
	{
		Settings::Internal::bHasScriptContainers = true;
		std::cerr << std::format(
			"ScriptContainers: FScriptMapHelper::FindOrAdd 0x{:X} | RemoveAt 0x{:X} | FindPairIndex 0x{:X} | "
			"FScriptSetHelper::RemoveElement 0x{:X} | RemoveAt 0x{:X} | AddElement 0x{:X}\n",
			Off::InSDK::ScriptContainers::FScriptMapHelperFindOrAddOffset,
			Off::InSDK::ScriptContainers::FScriptMapHelperRemoveAtOffset,
			Off::InSDK::ScriptContainers::FScriptMapHelperFindPairIndexOffset,
			Off::InSDK::ScriptContainers::FScriptSetHelperRemoveElementOffset,
			Off::InSDK::ScriptContainers::FScriptSetHelperRemoveAtOffset,
			Off::InSDK::ScriptContainers::FScriptSetHelperAddElementOffset);
	}
#endif
#endif
}

/*
* Off::InSDK::WeakObject::InitAllocateSerialNumber
*
* FUObjectArray::AllocateSerialNumber(this=GUObjectArray, int32 Index) -> int32 serial. The prologue +
* FUObjectItem (0x18 stride) index math is rip-relative-free, but MSVC emits the index->chunk math two
* different ways across builds, so we carry one signature per family (frame-size + rel8 'jge' masked):
*   A (UE5.6 / RC): push rdi; sub rsp,20h; cmp edx,[rcx+24h](NumElements); jge ..; mov r8,[rcx+10h](Objects);
*     movzx eax,dx; shr r9,10h; lea rdx,[rax+rax*2]; mov rax,[r8+r9*8]; lea rdi,[rax+rdx*8].
*   B (UE4.27 / DRG): push rdi; sub rsp,30h; mov r9,rcx; cmp edx,[rcx+24h]; jge ..; signed split
*     (mov eax,edx; cdq; movzx edx,dx; add eax,edx; movzx eax,ax; sub eax,edx; sar r8d,10h; ...);
*     mov rdx,[rcx+10h](Objects); lea rcx,[rax+rax*2]; mov rax,[rdx+r8*8]; lea rdi,[rax+rcx*8].
* GUObjectArray layout (Objects @+0x10, NumElements @+0x24) is shared; only the codegen + RVA differ.
*/
void Off::InSDK::WeakObject::InitAllocateSerialNumber()
{
#ifdef PLATFORM_WINDOWS
#if defined(_WIN64)
	const char* const sigs[] = {
		// A: UE5.6 / RogueCore
		"40 57 48 83 EC 20 3B 51 24 7D ?? 4C 8B 41 10 44 8B CA 0F B7 C2 49 C1 E9 10 48 8D 14 40 4B 8B 04 C8 48 8D 3C D0",
		// B: UE4.27 / Deep Rock Galactic
		"40 57 48 83 EC ?? 4C 8B C9 3B 51 24 7D ?? 8B C2 99 0F B7 D2 03 C2 44 8B C0 0F B7 C0 2B C2 48 8B 51 10 48 98 41 C1 F8 10 4D 63 C0 48 8D 0C 40 4A 8B 04 C2 48 8D 3C C8",
	};

	void* Fn = nullptr;
	for (const char* sig : sigs)
	{
		Fn = Platform::FindPattern(sig, 0x0, false, 0x0, nullptr);
		if (Fn)
			break;
	}
	if (!Fn)
	{
		std::cerr << "FUObjectArray::AllocateSerialNumber: pattern not matched, skipping (TWeakObjectPtr builder unavailable).\n";
		return;
	}

	Off::InSDK::WeakObject::AllocateSerialNumberOffset =
		static_cast<int32>(reinterpret_cast<uint8_t*>(Fn) - reinterpret_cast<uint8_t*>(Platform::GetModuleBase()));
	Settings::Internal::bHasAllocateSerialNumber = true;
	std::cerr << std::format("FUObjectArray::AllocateSerialNumber: 0x{:X}\n", Off::InSDK::WeakObject::AllocateSerialNumberOffset);
#endif
#endif
}

/* UWorld */
void Off::InSDK::World::InitGWorld()
{
	UEClass UWorld = ObjectArray::FindClassFast("World");

	for (UEObject Obj : ObjectArray())
	{
		if (Obj.HasAnyFlags(EObjectFlags::ClassDefaultObject) || !Obj.IsA(UWorld))
			continue;

		/* Try to find a pointer to the word, aka UWorld** GWorld */
		auto Results = Platform::FindAllAlignedValuesInProcess(Obj.GetAddress());

		void* Result = nullptr;
		if (Results.size())
		{
			if (Results.size() == 1)
			{
				Result = Results[0];
			}
			else if (Results.size() == 2)
			{
				auto ObjAddress = reinterpret_cast<uintptr_t>(Obj.GetAddress());
				auto PossibleGWorld = reinterpret_cast<volatile uintptr_t*>(Results[0]);
				auto CurrentValue = *PossibleGWorld;

				for (int i = 0; CurrentValue == ObjAddress && i < 50; ++i)
				{
					::Sleep(1);
					CurrentValue = *PossibleGWorld;
				}
				if (CurrentValue == ObjAddress)
				{
					Result = Results[0];
				}
				else
				{
					Result = Results[1];
					std::cerr << std::format("Filter GActiveLogWorld at 0x{:X}\n\n", reinterpret_cast<uintptr_t>(PossibleGWorld));
				}
			}
			else
			{
				std::cerr << std::format("Detected {} GWorld \n\n", Results.size());
			}
		}

		/* Pointer to UWorld* couldn't be found */
		if (Result)
		{
			Off::InSDK::World::GWorld = static_cast<int32>(Platform::GetOffset(Result));
			std::cerr << std::format("GWorld-Offset: 0x{:X}\n\n", Off::InSDK::World::GWorld);
			break;
		}
	}

	if (Off::InSDK::World::GWorld == 0x0)
		std::cerr << std::format("\nGWorld WAS NOT FOUND!!!!!!!!!\n\n");
}

/* FText */
void Off::InSDK::Text::InitTextOffsets()
{
	if (!Off::InSDK::ProcessEvent::PEIndex)
	{
		std::cerr << std::format("\nDumper-7: Error, 'InitInSDKTextOffsets' was called before ProcessEvent was initialized!\n") << std::endl;
		return;
	}

	auto IsValidPtr = [](void* a) -> bool
		{
			return !Platform::IsBadReadPtr(a) /* && (uintptr_t(a) & 0x1) == 0*/; // realistically, there wont be any pointers to unaligned memory
		};


	const UEFunction Conv_StringToText = ObjectArray::FindObjectFast<UEFunction>("Conv_StringToText", EClassCastFlags::Function);

	UEProperty InStringProp = nullptr;
	UEProperty ReturnProp = nullptr;

	if (!Conv_StringToText)
	{
		std::cerr << "Conv_StringToText is invalid!\n";
		return;
	}

	for (UEProperty Prop : Conv_StringToText.GetProperties())
	{
		/* Func has 2 params, if the param is the return value assign to ReturnProp, else InStringProp*/
		if (Prop.HasPropertyFlags(EPropertyFlags::ReturnParm))
		{
			ReturnProp = Prop;
		}
		else
		{
			InStringProp = Prop;
		}
	}

	const int32 ParamSize = Conv_StringToText.GetStructSize();
	const int32 FTextSize = ReturnProp.GetSize();

	const int32 StringOffset = InStringProp.GetOffset();
	const int32 ReturnValueOffset = ReturnProp.GetOffset();

	Off::InSDK::Text::TextSize = FTextSize;


	/* Allocate and zero-initialize ParamStruct */
#pragma warning(disable: 6255)
	uint8_t* ParamPtr = static_cast<uint8_t*>(alloca(ParamSize));
	memset(ParamPtr, 0, ParamSize);

	/* Choose a, fairly random, string to later search for in FTextData */
	constexpr const wchar_t* StringText = L"ThisIsAGoodString!";
	constexpr int32 StringLength = (sizeof(L"ThisIsAGoodString!") / sizeof(wchar_t));
	constexpr int32 StringLengthBytes = (sizeof(L"ThisIsAGoodString!"));

	/* Initialize 'InString' in the ParamStruct */
	*reinterpret_cast<FString*>(ParamPtr + StringOffset) = StringText;

	/* This function is 'static' so the object on which we call it doesn't matter */
	ObjectArray::GetByIndex(0).ProcessEvent(Conv_StringToText, ParamPtr);

	uint8_t* FTextDataPtr = nullptr;

	/* Search for the first valid pointer inside of the FText and make the offset our 'TextDatOffset' */
	for (int32 i = 0; i < (FTextSize - sizeof(void*)); i += sizeof(void*))
	{
		void* PossibleTextDataPtr = *reinterpret_cast<void**>(ParamPtr + ReturnValueOffset + i);

		if (IsValidPtr(PossibleTextDataPtr))
		{
			FTextDataPtr = static_cast<uint8_t*>(PossibleTextDataPtr);
			Off::InSDK::Text::TextDatOffset = i;
			break;
		}
	}

	if (!FTextDataPtr)
	{
		std::cerr << std::format("\nDumper-7: Error, 'FTextDataPtr' could not be found!\n") << std::endl;
		return;
	}

	constexpr int32 MaxOffset = 0x50;
	constexpr int32 StartOffset = sizeof(void*); // FString::NumElements offset

	/* Search for a pointer pointing to a int32 Value (FString::NumElements) equal to StringLength */
	for (int32 i = StartOffset; i < MaxOffset; i += sizeof(int32))
	{
		wchar_t* PosibleStringPtr = *reinterpret_cast<wchar_t**>((FTextDataPtr + i) - sizeof(void*));
		const int32 PossibleLength = *reinterpret_cast<int32*>(FTextDataPtr + i);

		if (PossibleLength == StringLength && PosibleStringPtr && IsValidPtr(PosibleStringPtr) && memcmp(StringText, PosibleStringPtr, StringLengthBytes) == 0)
		{
			Off::InSDK::Text::InTextDataStringOffset = (i - sizeof(void*));
			break;
		}
	}

	std::cerr << std::format("Off::InSDK::Text::TextSize: 0x{:X}\n", Off::InSDK::Text::TextSize);
	std::cerr << std::format("Off::InSDK::Text::TextDatOffset: 0x{:X}\n", Off::InSDK::Text::TextDatOffset);
	std::cerr << std::format("Off::InSDK::Text::InTextDataStringOffset: 0x{:X}\n\n", Off::InSDK::Text::InTextDataStringOffset);
}

void Off::Init()
{
	auto OverwriteIfInvalidOffset = [](int32& Offset, int32 DefaultValue)
		{
			if (Offset == OffsetFinder::OffsetNotFound)
			{
				std::cerr << std::format("Defaulting to offset: 0x{:X}\n", DefaultValue);
				Offset = DefaultValue;
			}
		};

	Off::UObject::Flags = OffsetFinder::FindUObjectFlagsOffset();
	OverwriteIfInvalidOffset(Off::UObject::Flags, sizeof(void*)); // Default to right after VTable
	std::cerr << std::format("Off::UObject::Flags: 0x{:X}\n", Off::UObject::Flags);

	Off::UObject::Index = OffsetFinder::FindUObjectIndexOffset();
	OverwriteIfInvalidOffset(Off::UObject::Index, (Off::UObject::Flags + sizeof(int32))); // Default to right after Flags
	std::cerr << std::format("Off::UObject::Index: 0x{:X}\n", Off::UObject::Index);

	Off::UObject::Class = OffsetFinder::FindUObjectClassOffset();
	OverwriteIfInvalidOffset(Off::UObject::Class, (Off::UObject::Index + sizeof(int32))); // Default to right after Index
	std::cerr << std::format("Off::UObject::Class: 0x{:X}\n", Off::UObject::Class);

	Off::UObject::Outer = OffsetFinder::FindUObjectOuterOffset();
	std::cerr << std::format("Off::UObject::Outer: 0x{:X}\n", Off::UObject::Outer);

	Off::UObject::Name = OffsetFinder::FindUObjectNameOffset();
	OverwriteIfInvalidOffset(Off::UObject::Name, (Off::UObject::Class + sizeof(void*))); // Default to right after Class
	std::cerr << std::format("Off::UObject::Name: 0x{:X}\n\n", Off::UObject::Name);

	OverwriteIfInvalidOffset(Off::UObject::Outer, (Off::UObject::Name + sizeof(int32) + sizeof(int32)));  // Default to right after Name

	OffsetFinder::InitFNameSettings();

	::NameArray::PostInit();

	// Castflags needs to stay here since the FindChildOffset() uses CastFlags
	Off::UClass::CastFlags = OffsetFinder::FindCastFlagsOffset();
	std::cerr << std::format("Off::UClass::CastFlags: 0x{:X}\n", Off::UClass::CastFlags);

	Off::UStruct::Children = OffsetFinder::FindChildOffset();
	std::cerr << std::format("Off::UStruct::Children: 0x{:X}\n", Off::UStruct::Children);

	Off::UField::Next = OffsetFinder::FindUFieldNextOffset();
	std::cerr << std::format("Off::UField::Next: 0x{:X}\n", Off::UField::Next);

	Off::UStruct::SuperStruct = OffsetFinder::FindSuperOffset();
	std::cerr << std::format("Off::UStruct::SuperStruct: 0x{:X}\n", Off::UStruct::SuperStruct);

	Off::UStruct::Size = OffsetFinder::FindStructSizeOffset();
	std::cerr << std::format("Off::UStruct::Size: 0x{:X}\n", Off::UStruct::Size);

	Off::UStruct::MinAlignment = OffsetFinder::FindMinAlignmentOffset();
	std::cerr << std::format("Off::UStruct::MinAlignment: 0x{:X}\n", Off::UStruct::MinAlignment);

	Off::UClass::CastFlags = OffsetFinder::FindCastFlagsOffset();
	std::cerr << std::format("Off::UClass::CastFlags: 0x{:X}\n", Off::UClass::CastFlags);

	// Castflags become available for use

	// UStruct::Script (BP bytecode) + StructBaseChain (O(1) IsChildOf) - derived from binary, not emitted by older Dumper-7.
	// Script needs MinAlignment (+ CastFlags for the IsA(Function) verify); StructBaseChain needs SuperStruct - all set above.
	Off::UStruct::Script = OffsetFinder::FindScriptOffset();
	std::cerr << std::format("Off::UStruct::Script: 0x{:X}\n", Off::UStruct::Script);

	Off::UStruct::StructBaseChainArray = OffsetFinder::FindStructBaseChainOffset();
	if (Off::UStruct::StructBaseChainArray != OffsetFinder::OffsetNotFound)
		Off::UStruct::NumStructBasesInChainMinusOne = Off::UStruct::StructBaseChainArray + sizeof(void*);
	std::cerr << std::format("Off::UStruct::StructBaseChainArray: 0x{:X} (NumStructBasesInChainMinusOne @ 0x{:X})\n",
		Off::UStruct::StructBaseChainArray, Off::UStruct::NumStructBasesInChainMinusOne);

	if (Settings::Internal::bUseFProperty)
	{
		std::cerr << std::format("\nGame uses FProperty system\n\n");

		Off::UStruct::ChildProperties = OffsetFinder::FindChildPropertiesOffset();
		std::cerr << std::format("Off::UStruct::ChildProperties: 0x{:X}\n", Off::UStruct::ChildProperties);

		OffsetFinder::FixupHardcodedOffsets(); // must be called after FindChildPropertiesOffset 

		Off::FField::Next = OffsetFinder::FindFFieldNextOffset();
		std::cerr << std::format("Off::FField::Next: 0x{:X}\n", Off::FField::Next);

		Off::FField::Class = OffsetFinder::FindFFieldClassOffset();
		std::cerr << std::format("Off::FField::Class: 0x{:X}\n", Off::FField::Class);

		// Comment out this line if you're crashing here and see if the NewFindFFieldNameOffset might work!
		Off::FField::Name = OffsetFinder::FindFFieldNameOffset();
		//Off::FField::Name = OffsetFinder::NewFindFFieldNameOffset();

		if (Off::FField::Name == OffsetFinder::OffsetNotFound)
			Off::FField::Name = OffsetFinder::NewFindFFieldNameOffset();

		std::cerr << std::format("Off::FField::Name: 0x{:X}\n", Off::FField::Name);

		/*
		* FNameSize might be wrong at this point of execution.
		* FField::Flags is not critical so a fix is only applied later in OffsetFinder::PostInitFNameSettings().
		*/
		Off::FField::Flags = Off::FField::Name + Off::InSDK::Name::FNameSize;
		std::cerr << std::format("Off::FField::Flags: 0x{:X}\n", Off::FField::Flags);
	}

	Off::UClass::ClassDefaultObject = OffsetFinder::FindDefaultObjectOffset();
	std::cerr << std::format("Off::UClass::ClassDefaultObject: 0x{:X}\n", Off::UClass::ClassDefaultObject);

	Off::UClass::ImplementedInterfaces = OffsetFinder::FindImplementedInterfacesOffset();
	std::cerr << std::format("Off::UClass::ImplementedInterfaces: 0x{:X}\n", Off::UClass::ImplementedInterfaces);

	Off::UEnum::Names = OffsetFinder::FindEnumNamesOffset();
	std::cerr << std::format("Off::UEnum::Names: 0x{:X}\n", Off::UEnum::Names) << std::endl;

	Off::UFunction::FunctionFlags = OffsetFinder::FindFunctionFlagsOffset();
	std::cerr << std::format("Off::UFunction::FunctionFlags: 0x{:X}\n", Off::UFunction::FunctionFlags);

	Off::UFunction::ExecFunction = OffsetFinder::FindFunctionNativeFuncOffset();
	std::cerr << std::format("Off::UFunction::ExecFunction: 0x{:X}\n", Off::UFunction::ExecFunction) << std::endl;

	Off::Property::ElementSize = OffsetFinder::FindElementSizeOffset();
	std::cerr << std::format("Off::Property::ElementSize: 0x{:X}\n", Off::Property::ElementSize);

	Off::Property::ArrayDim = OffsetFinder::FindArrayDimOffset();
	std::cerr << std::format("Off::Property::ArrayDim: 0x{:X}\n", Off::Property::ArrayDim);

	Off::Property::Offset_Internal = OffsetFinder::FindOffsetInternalOffset();
	std::cerr << std::format("Off::Property::Offset_Internal: 0x{:X}\n", Off::Property::Offset_Internal);

	Off::Property::PropertyFlags = OffsetFinder::FindPropertyFlagsOffset();
	std::cerr << std::format("Off::Property::PropertyFlags: 0x{:X}\n", Off::Property::PropertyFlags);

	Off::Property::RepIndex = OffsetFinder::FindRepIndexOffset(Off::Property::PropertyFlags);
	std::cerr << std::format("Off::Property::RepIndex: 0x{:X}\n", Off::Property::RepIndex);

	Off::Property::BlueprintReplicationCondition = OffsetFinder::FindBlueprintReplicationConditionOffset(Off::Property::RepIndex);
	std::cerr << std::format("Off::Property::BlueprintReplicationCondition: 0x{:X}\n", Off::Property::BlueprintReplicationCondition);

	Off::Property::RepNotifyFunc = OffsetFinder::FindRepNotifyFuncOffset(Off::Property::Offset_Internal);
	std::cerr << std::format("Off::Property::RepNotifyFunc: 0x{:X}\n", Off::Property::RepNotifyFunc);

	Off::Property::PropertyLinkNext = OffsetFinder::FindPropertyLinkNextOffset(Off::Property::RepNotifyFunc);
	std::cerr << std::format("Off::Property::PropertyLinkNext: 0x{:X}\n", Off::Property::PropertyLinkNext);

	Off::Property::NextRef = OffsetFinder::FindNextRefOffset(Off::Property::PropertyLinkNext);
	std::cerr << std::format("Off::Property::NextRef: 0x{:X}\n", Off::Property::NextRef);

	Off::Property::DestructorLinkNext = OffsetFinder::FindDestructorLinkNextOffset(Off::Property::NextRef);
	std::cerr << std::format("Off::Property::DestructorLinkNext: 0x{:X}\n", Off::Property::DestructorLinkNext);

	Off::Property::PostConstructLinkNext = OffsetFinder::FindPostConstructLinkNextOffset(Off::Property::DestructorLinkNext);
	std::cerr << std::format("Off::Property::PostConstructLinkNext: 0x{:X}\n", Off::Property::PostConstructLinkNext);

	Off::BoolProperty::Base = OffsetFinder::FindBoolPropertyBaseOffset();
	std::cerr << std::format("UBoolProperty::Base: 0x{:X}\n", Off::BoolProperty::Base) << std::endl;

	Off::EnumProperty::Base = OffsetFinder::FindEnumPropertyBaseOffset();
	std::cerr << std::format("Off::EnumProperty::Base: 0x{:X}\n", Off::EnumProperty::Base) << std::endl;


	if (Off::EnumProperty::Base == OffsetFinder::OffsetNotFound)
	{
		Off::InSDK::Properties::PropertySize = Off::BoolProperty::Base;
		Off::EnumProperty::Base = Off::BoolProperty::Base;
	}
	else
	{
		Off::InSDK::Properties::PropertySize = Off::EnumProperty::Base;
	}

	std::cerr << std::format("UPropertySize: 0x{:X}\n", Off::InSDK::Properties::PropertySize) << std::endl;

	Off::ObjectProperty::PropertyClass = OffsetFinder::FindObjectPropertyClassOffset();
	std::cerr << std::format("Off::ObjectProperty::PropertyClass: 0x{:X}", Off::ObjectProperty::PropertyClass) << std::endl;
	OverwriteIfInvalidOffset(Off::ObjectProperty::PropertyClass, Off::InSDK::Properties::PropertySize);

	Off::ByteProperty::Enum = OffsetFinder::FindBytePropertyEnumOffset();
	OverwriteIfInvalidOffset(Off::ByteProperty::Enum, Off::InSDK::Properties::PropertySize);
	std::cerr << std::format("Off::ByteProperty::Enum: 0x{:X}", Off::ByteProperty::Enum) << std::endl;

	Off::StructProperty::Struct = OffsetFinder::FindStructPropertyStructOffset();
	OverwriteIfInvalidOffset(Off::StructProperty::Struct, Off::InSDK::Properties::PropertySize);
	std::cerr << std::format("Off::StructProperty::Struct: 0x{:X}\n", Off::StructProperty::Struct) << std::endl;

	Off::DelegateProperty::SignatureFunction = OffsetFinder::FindDelegatePropertySignatureFunctionOffset();
	OverwriteIfInvalidOffset(Off::DelegateProperty::SignatureFunction, Off::InSDK::Properties::PropertySize);
	std::cerr << std::format("Off::DelegateProperty::SignatureFunction: 0x{:X}\n", Off::DelegateProperty::SignatureFunction) << std::endl;

	Off::ArrayProperty::Inner = OffsetFinder::FindInnerTypeOffset(Off::InSDK::Properties::PropertySize);
	std::cerr << std::format("Off::ArrayProperty::Inner: 0x{:X}\n", Off::ArrayProperty::Inner);

	Off::SetProperty::ElementProp = OffsetFinder::FindSetPropertyBaseOffset(Off::InSDK::Properties::PropertySize);
	std::cerr << std::format("Off::SetProperty::ElementProp: 0x{:X}\n", Off::SetProperty::ElementProp);

	Off::MapProperty::Base = OffsetFinder::FindMapPropertyBaseOffset(Off::InSDK::Properties::PropertySize);
	std::cerr << std::format("Off::MapProperty::Base: 0x{:X}\n", Off::MapProperty::Base) << std::endl;

	Off::InSDK::ULevel::Actors = OffsetFinder::FindLevelActorsOffset();
	std::cerr << std::format("Off::InSDK::ULevel::Actors: 0x{:X}\n", Off::InSDK::ULevel::Actors) << std::endl;

	Off::InSDK::UDataTable::RowMap = OffsetFinder::FindDatatableRowMapOffset();
	std::cerr << std::format("Off::InSDK::UDataTable::RowMap: 0x{:X}\n", Off::InSDK::UDataTable::RowMap) << std::endl;

	Off::InterfaceProperty::InterfaceClass = Off::DelegateProperty::SignatureFunction;
	std::cerr << std::format("Off::InterfaceProperty::InterfaceClass: 0x{:X}\n", Off::InterfaceProperty::InterfaceClass) << std::endl;

	Off::MulticastDelegateProperty::SignatureFunction = Off::DelegateProperty::SignatureFunction;
	std::cerr << std::format("Off::MulticastDelegateProperty::SignatureFunction: 0x{:X}\n", Off::MulticastDelegateProperty::SignatureFunction) << std::endl;

	OffsetFinder::PostInitFNameSettings();

	std::cerr << std::endl;

	Off::FieldPathProperty::FieldClass = OffsetFinder::FindFieldPathPropertyFieldClassOffset(Off::InSDK::Properties::PropertySize);
	Off::OptionalProperty::ValueProperty = OffsetFinder::FindOptionalPropertyValuePropertyOffset(Off::InSDK::Properties::PropertySize);

	Off::ClassProperty::MetaClass = Off::ObjectProperty::PropertyClass + sizeof(void*); //0x8 inheritance from ObjectProperty
}

void PropertySizes::Init()
{
	InitTDelegateSize();
	InitFFieldPathSize();
	InitTMulticastInlineDelegateSize();
}

void PropertySizes::InitTDelegateSize()
{
	/* If the AudioComponent class or the OnQueueSubtitles member weren't found, fallback to looping GObjects and looking for a Delegate. */
	auto OnPropertyNotFound = [&]() -> void
		{
			for (UEObject Obj : ObjectArray())
			{
				if (!Obj.IsA(EClassCastFlags::Struct))
					continue;

				for (UEProperty Prop : Obj.Cast<UEClass>().GetProperties())
				{
					if (Prop.IsA(EClassCastFlags::DelegateProperty))
					{
						PropertySizes::DelegateProperty = Prop.GetSize();
						return;
					}
				}
			}
		};

	const UEClass AudioComponentClass = ObjectArray::FindClassFast("AudioComponent");

	if (!AudioComponentClass)
		return OnPropertyNotFound();

	const UEProperty OnQueueSubtitlesProp = AudioComponentClass.FindMember("OnQueueSubtitles", EClassCastFlags::DelegateProperty);

	if (!OnQueueSubtitlesProp)
		return OnPropertyNotFound();

	PropertySizes::DelegateProperty = OnQueueSubtitlesProp.GetSize();
}

void PropertySizes::InitFFieldPathSize()
{
	if (!Settings::Internal::bUseFProperty)
		return;

	/* If the SetFieldPathPropertyByName function or the Value parameter weren't found, fallback to looping GObjects and looking for a Delegate. */
	auto OnPropertyNotFound = [&]() -> void
		{
			for (UEObject Obj : ObjectArray())
			{
				if (!Obj.IsA(EClassCastFlags::Struct))
					continue;

				for (UEProperty Prop : Obj.Cast<UEClass>().GetProperties())
				{
					if (Prop.IsA(EClassCastFlags::FieldPathProperty))
					{
						PropertySizes::FieldPathProperty = Prop.GetSize();
						return;
					}
				}
			}
		};

	const UEFunction SetFieldPathPropertyByNameFunc = ObjectArray::FindObjectFast<UEFunction>("SetFieldPathPropertyByName", EClassCastFlags::Function);

	if (!SetFieldPathPropertyByNameFunc)
		return OnPropertyNotFound();

	const UEProperty ValueParamProp = SetFieldPathPropertyByNameFunc.FindMember("Value", EClassCastFlags::FieldPathProperty);

	if (!ValueParamProp)
		return OnPropertyNotFound();

	PropertySizes::FieldPathProperty = ValueParamProp.GetSize();
}

void PropertySizes::InitTMulticastInlineDelegateSize()
{
	/* If the AudioComponent class or the OnQueueSubtitles member weren't found, fallback to looping GObjects and looking for a Delegate. */
	auto OnPropertyNotFound = [&]() -> void
		{
			for (UEObject Obj : ObjectArray())
			{
				if (!Obj.IsA(EClassCastFlags::Struct))
					continue;

				for (UEProperty Prop : Obj.Cast<UEClass>().GetProperties())
				{
					if (Prop.IsA(EClassCastFlags::MulticastInlineDelegateProperty))
					{
						PropertySizes::DelegateProperty = Prop.GetSize();
						return;
					}
				}
			}
		};

	const UEClass EmitterClass = ObjectArray::FindClassFast("Emitter");

	if (!EmitterClass)
		return OnPropertyNotFound();

	const UEProperty OnParticleSpawn = EmitterClass.FindMember("OnParticleSpawn", EClassCastFlags::MulticastDelegateProperty);

	if (!OnParticleSpawn)
		return OnPropertyNotFound();

	PropertySizes::MulticastInlineDelegateProperty = OnParticleSpawn.GetSize();
}