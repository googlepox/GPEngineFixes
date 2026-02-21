// ============================================================================
//  MediumArmor OBSE Plugin – Hooks.cpp
//
//  Four hooks that make medium armor work throughout the engine:
//
//  Hook 1 — sub_488CB0 (per-piece combat AR wrapper)
//      Bypasses vanilla skill lookup; uses GetMediumArmorSkill() directly.
//
//  Hook 2 — IsHeavyArmor
//      Returns false for medium armor so engine classifies it as non-heavy.
//
//  Hook 3 — GetArmorSkillAV (flag setter)
//      Sets a thread-local flag + caches medium skill value when a medium
//      piece is queried.  Still returns a valid AV code so the caller's
//      GetActorValue doesn't crash.
//
//  Hook 4 — Calc_ArmorRating (flag consumer)
//      Checks the flag.  If set, replaces the skill parameter on the stack
//      with the cached medium skill, clears the flag, then runs the original.
//      This fixes the inventory display AR and any other code path that does
//      GetArmorSkillAV → GetActorValue → Calc_ArmorRating.
//
//  All addresses: Oblivion 1.2.0.416 (GOTY / Steam).
// ============================================================================

#include "Hooks.h"
#include "MediumArmor.h"
#include "Config.h"

#include "obse/GameAPI.h"
#include "obse/GameObjects.h"
#include "obse/GameForms.h"
#include "obse_common/SafeWrite.h"

#include <cmath>
#include <algorithm>
#include <windows.h>
#include <obse_common/SafeWrite.h>

namespace MediumArmor
{

    // ════════════════════════════════════════════════════════════════════════════
    //  Address table  (Oblivion 1.2.0.416)
    // ════════════════════════════════════════════════════════════════════════════

    namespace Addr
    {
        // Hook 1: per-piece combat AR wrapper
        constexpr UInt32 Sub_488CB0 = 0x00488CB0;
        constexpr UInt32 Sub_488CB0_Resume = 0x00488CB9;  // push ebp

        // Hook 2: heavy armor classification
        constexpr UInt32 IsHeavyArmor = 0x004B4C70;  // 7 bytes

        // Hook 3: armor skill AV code lookup
        constexpr UInt32 GetArmorSkillAV = 0x004B4C80;  // 16 bytes (0x10)

        // Hook 4: AR formula
        constexpr UInt32 Calc_ArmorRating = 0x00547370;  // 0x123 bytes

        // Float constant loaded in sub_488CB0 stolen prologue
        constexpr UInt32 FloatConst = 0x00A30634;

        // Call sites inside sub_488CB0 — we extract targets at init
        constexpr UInt32 CallSite_GetHealthForForm = 0x00488D02;
        constexpr UInt32 CallSite_GetHealth = 0x00488D35;
    }

    // ════════════════════════════════════════════════════════════════════════════
    //  Byte counts for stolen regions
    // ════════════════════════════════════════════════════════════════════════════

    constexpr UInt32 kStolenBytes_488CB0 = 9;   // sub esp,0Ch + fld [...]
    constexpr UInt32 kStolenBytes_IsHeavyArmor = 7;   // mov al,[ecx+6Ah] + shr al,7 + retn
    constexpr UInt32 kStolenBytes_SkillAV = 16;  // entire function (0x10 bytes)
    // Calc_ArmorRating stolen bytes — set after prologue dump
    static UInt32    s_stolenBytes_CalcAR = 0;

    // ════════════════════════════════════════════════════════════════════════════
    //  Vanilla function pointer types
    // ════════════════════════════════════════════════════════════════════════════

    typedef double(__cdecl* Calc_ArmorRating_t)(unsigned short, float, float, float);
    typedef int(__cdecl* GetHealthForForm_t)(void*);
    typedef float(__thiscall* GetHealth_t)(void*, int);

    // ════════════════════════════════════════════════════════════════════════════
    //  Resolved vanilla function pointers  (set in InstallHooks)
    // ════════════════════════════════════════════════════════════════════════════

    static Calc_ArmorRating_t fn_CalcArmorRating = nullptr;
    static GetHealthForForm_t fn_GetHealthForForm = nullptr;
    static GetHealth_t        fn_GetHealth = nullptr;

    // ════════════════════════════════════════════════════════════════════════════
    //  State
    // ════════════════════════════════════════════════════════════════════════════

    static UInt8 s_origBytes_488CB0[kStolenBytes_488CB0];
    static UInt8 s_origBytes_IHA[kStolenBytes_IsHeavyArmor];
    static UInt8 s_origBytes_SkillAV[kStolenBytes_SkillAV];
    static UInt8 s_origBytes_CalcAR[16];  // max we'd ever steal
    static bool  s_hookInstalled = false;

    // ════════════════════════════════════════════════════════════════════════════
    //  Helpers
    // ════════════════════════════════════════════════════════════════════════════

    static UInt32 ExtractCallTarget(UInt32 callSiteAddr)
    {
        SInt32 offset = *(SInt32*)(callSiteAddr + 1);
        return callSiteAddr + 5 + offset;
    }

    // ════════════════════════════════════════════════════════════════════════════
    //  CalcMediumPieceAR  (used by Hook 1 — combat path)
    // ════════════════════════════════════════════════════════════════════════════

    static float __cdecl CalcMediumPieceAR(int equippedInstance, void* actor)
    {
        void* armorForm = *(void**)(equippedInstance + 0x8);
        if (!armorForm)
            return 0.0f;

        typedef float(__thiscall* GetActorValue_fn)(void*, int);
        UInt32 vtable = *(UInt32*)actor;
        GetActorValue_fn fnGetAV = *(GetActorValue_fn*)(vtable + 0x288);

        float luck = fnGetAV(actor, 7);

        float skill = GetMediumArmorSkill() * kARMultiplier + kARFlat;
        skill = std::clamp(skill, 0.0f, 100.0f);

        float condition = 0.0f;
        int maxHP = fn_GetHealthForForm(armorForm);
        if (maxHP != 0)
        {
            float maxHPf = static_cast<float>(maxHP);
            if (maxHP < 0)
                maxHPf += 4294967296.0f;
            condition = fn_GetHealth(reinterpret_cast<void*>(equippedInstance), 0) / maxHPf;
        }

        UInt16 rawAR = *(UInt16*)((UInt8*)armorForm + 0xE4);
        UInt16 baseAR = static_cast<UInt16>(static_cast<double>(rawAR) / 100.0);

        double result = fn_CalcArmorRating(baseAR, skill, luck, condition);

        _MESSAGE("MediumArmor AR (combat): baseAR=%u skill=%.1f luck=%.1f cond=%.3f -> %.1f",
            baseAR, skill, luck, condition, result);

        float fresult = static_cast<float>(result);
        float truncated = static_cast<float>(static_cast<int>(fresult));
        if (truncated - fresult < 0.0f)
            truncated += 1.0f;

        return truncated;
    }

    // ════════════════════════════════════════════════════════════════════════════
    //  Close namespace for file-scope ASM-visible globals
    // ════════════════════════════════════════════════════════════════════════════

}  // close MediumArmor namespace temporarily

// ── ASM-callable function pointers ─────────────────────────────────────────
static bool(__cdecl* s_fnIsMediumArmor)(TESForm*) = nullptr;
static float(__cdecl* s_fnCalcMediumPieceAR)(int, void*) = nullptr;
static UInt32 s_resumeAddr_488CB0 = 0;
static UInt32 s_resumeAddr_CalcAR = 0;

// ── Medium armor flag (set by Hook 3, consumed by Hook 4) ──────────────────
static bool  s_mediumArmorFlag = false;
static float s_mediumSkillValue = 0.0f;

// ── Helper: get medium skill with modifiers applied ────────────────────────
static float __cdecl GetMediumSkillWithMods()
{
    float skill = MediumArmor::GetMediumArmorSkill()
        * MediumArmor::kARMultiplier
        + MediumArmor::kARFlat;
    return std::clamp(skill, 0.0f, 100.0f);
}
static float(__cdecl* s_fnGetMediumSkill)() = &GetMediumSkillWithMods;

// ════════════════════════════════════════════════════════════════════════════
//  Hook 2 — IsHeavyArmor detour  (0x004B4C70)
//  Medium → return false.  Otherwise → original logic.
// ════════════════════════════════════════════════════════════════════════════

static __declspec(naked) void Detour_IsHeavyArmor()
{
    __asm
    {
        push    ecx
        push    ecx
        call[s_fnIsMediumArmor]
        add     esp, 4
        test    al, al
        pop     ecx
        jnz     is_medium

        mov     al, [ecx + 0x6A]
        shr     al, 7
        ret

        is_medium :
        xor al, al
            ret
    }
}

// ════════════════════════════════════════════════════════════════════════════
//  Hook 3 — GetArmorSkillAV detour  (0x004B4C80)
//  Medium → set flag + cache skill, return kActorVal_LightArmor (0x1B).
//  Otherwise → original branchless logic.
// ════════════════════════════════════════════════════════════════════════════

static __declspec(naked) void Detour_GetArmorSkillAV()
{
    __asm
    {
        // ECX = TESObjectARMO* (thiscall)
        push    ecx
        push    ecx                             // arg: TESForm*
        call[s_fnIsMediumArmor]             // bool __cdecl
        add     esp, 4
        test    al, al
        pop     ecx
        jnz     medium_skill

        // ── Not medium: original logic ─────────────────────────────────────
        mov     al, [ecx + 0x6A]
        and al, 0x80
        neg     al
        sbb     eax, eax
        and eax, 0FFFFFFF7h
        add     eax, 1Bh
        ret

        // ── Medium: set flag + cache skill, return light AV code ───────────
        medium_skill :
        mov     byte ptr[s_mediumArmorFlag], 1

            // Cache the skill value including our multiplier/flat modifiers.
            // We can't easily call C++ from naked ASM, so we store a pre-computed
            // value that InstallHooks updates.  But GetMediumArmorSkill() can
            // change at any time (SetMediumArmorSkill console cmd), so we need
            // to read it live.  Use a minimal inline approach:
            push    ecx
            push    edx
            call[s_fnGetMediumSkill]            // float __cdecl → ST(0)
            fstp    dword ptr[s_mediumSkillValue]  // store to static
            pop     edx
            pop     ecx

            mov     eax, 1Bh                        // return kActorVal_LightArmor
            ret
    }
}

// ════════════════════════════════════════════════════════════════════════════
//  Hook 4 — Calc_ArmorRating detour  (0x00547370)
//  If flag set: replace skill param on stack, clear flag, fall through.
//  Stack layout at entry (cdecl):
//      [ESP+0]  = return address
//      [ESP+4]  = a1: baseAR  (uint16)
//      [ESP+8]  = a2: skill   (float)  ← we replace this
//      [ESP+C]  = a3: luck    (float)
//      [ESP+10] = a4: condition (float)
// ════════════════════════════════════════════════════════════════════════════

static __declspec(naked) void Detour_CalcArmorRating()
{
    __asm
    {
        cmp     byte ptr[s_mediumArmorFlag], 0
        jz      no_swap

        // ── Medium: swap skill param and clear flag ────────────────────────
        mov     byte ptr[s_mediumArmorFlag], 0
        push    eax
        mov     eax, dword ptr[s_mediumSkillValue]
        mov[esp + 0x0C], eax               // [esp+8+4] because we pushed eax
        pop     eax

        no_swap :
        // ── Execute stolen prologue bytes, then jump to resume ─────────────
        //    Filled at install time by copying the first N bytes.
        //    PLACEHOLDER: we use push/ret to jump to a C++ trampoline
        //    that replays the stolen bytes.  See InstallHooks().
        jmp[s_resumeAddr_CalcAR]
    }
}

// ════════════════════════════════════════════════════════════════════════════
//  Hook 1 — sub_488CB0 detour (combat AR wrapper)
// ════════════════════════════════════════════════════════════════════════════

static __declspec(naked) void Detour_Sub488CB0()
{
    __asm
    {
        push    ecx
        mov     eax, [ecx + 0x8]
        push    eax
        call[s_fnIsMediumArmor]
        add     esp, 4
        test    al, al
        pop     ecx
        jnz     medium_path

        vanilla_path :
        sub     esp, 0x0C
            fld     dword ptr ds : [0x00A30634]
            jmp[s_resumeAddr_488CB0]

            medium_path :
            push    dword ptr[esp + 0x4]
            push    ecx
            call[s_fnCalcMediumPieceAR]
            add     esp, 8
            ret     4
    }
}

// ════════════════════════════════════════════════════════════════════════════
//  Reopen namespace
// ════════════════════════════════════════════════════════════════════════════

namespace MediumArmor
{

    // ════════════════════════════════════════════════════════════════════════════
    //  Public API
    // ════════════════════════════════════════════════════════════════════════════

    bool InstallHooks()
    {
        // ── Init ASM-callable pointers ─────────────────────────────────────────
        s_fnIsMediumArmor = &IsMediumArmor;
        s_fnCalcMediumPieceAR = &CalcMediumPieceAR;
        s_resumeAddr_488CB0 = Addr::Sub_488CB0_Resume;

        // ── Resolve vanilla function pointers ──────────────────────────────────
        fn_CalcArmorRating = reinterpret_cast<Calc_ArmorRating_t>(Addr::Calc_ArmorRating);
        fn_GetHealthForForm = reinterpret_cast<GetHealthForForm_t>(
            ExtractCallTarget(Addr::CallSite_GetHealthForForm));
        fn_GetHealth = reinterpret_cast<GetHealth_t>(
            ExtractCallTarget(Addr::CallSite_GetHealth));

        _MESSAGE("MediumArmor: Resolved function pointers:");
        _MESSAGE("  Calc_ArmorRating  = %08X", Addr::Calc_ArmorRating);
        _MESSAGE("  GetHealthForForm  = %08X", reinterpret_cast<UInt32>(fn_GetHealthForForm));
        _MESSAGE("  GetHealth         = %08X", reinterpret_cast<UInt32>(fn_GetHealth));

        // ════════════════════════════════════════════════════════════════════════
        //  Hook 1: sub_488CB0  (combat AR)
        // ════════════════════════════════════════════════════════════════════════
        {
            const UInt8 expected[] = {
                0x83, 0xEC, 0x0C,
                0xD9, 0x05, 0x34, 0x06, 0xA3, 0x00
            };
            if (memcmp(reinterpret_cast<void*>(Addr::Sub_488CB0),
                expected, kStolenBytes_488CB0) != 0)
            {
                _ERROR("MediumArmor: sub_488CB0 prologue mismatch. Aborting.");
                return false;
            }
            memcpy(s_origBytes_488CB0, reinterpret_cast<void*>(Addr::Sub_488CB0),
                kStolenBytes_488CB0);
            WriteRelJump(Addr::Sub_488CB0, reinterpret_cast<UInt32>(&Detour_Sub488CB0));
            for (UInt32 i = 5; i < kStolenBytes_488CB0; ++i)
                SafeWrite8(Addr::Sub_488CB0 + i, 0x90);
            _MESSAGE("MediumArmor: Hook 1 (sub_488CB0) installed.");
        }

        // ════════════════════════════════════════════════════════════════════════
        //  Hook 2: IsHeavyArmor
        // ════════════════════════════════════════════════════════════════════════
        {
            const UInt8 expected[] = {
                0x8A, 0x41, 0x6A,
                0xC0, 0xE8, 0x07,
                0xC3
            };
            if (memcmp(reinterpret_cast<void*>(Addr::IsHeavyArmor),
                expected, kStolenBytes_IsHeavyArmor) != 0)
            {
                _ERROR("MediumArmor: IsHeavyArmor mismatch — skipping.");
            }
            else
            {
                memcpy(s_origBytes_IHA, reinterpret_cast<void*>(Addr::IsHeavyArmor),
                    kStolenBytes_IsHeavyArmor);
                WriteRelJump(Addr::IsHeavyArmor, reinterpret_cast<UInt32>(&Detour_IsHeavyArmor));
                SafeWrite8(Addr::IsHeavyArmor + 5, 0x90);
                SafeWrite8(Addr::IsHeavyArmor + 6, 0x90);
                _MESSAGE("MediumArmor: Hook 2 (IsHeavyArmor) installed.");
            }
        }

        // ════════════════════════════════════════════════════════════════════════
        //  Hook 4: Calc_ArmorRating  (install BEFORE hook 3 — hook 3 depends on it)
        //
        //  Prologue (Oblivion 1.2.0.416):
        //    00547370: D9 44 24 0C        fld dword ptr [esp+0Ch]   ; 4 bytes
        //    00547374: E8 47 B5 43 00     call Calc_LuckModifiedSkill ; 5 bytes
        //    00547379: ...                ← resume here
        //
        //  We steal 9 bytes.  The trampoline must fix up the relative call.
        // ════════════════════════════════════════════════════════════════════════
        bool hook4_ok = false;
        {
            UInt8* p = reinterpret_cast<UInt8*>(Addr::Calc_ArmorRating);

            // Verify the expected prologue
            const UInt8 expectedPrologue[] = {
                0xD9, 0x44, 0x24, 0x0C,                 // fld dword ptr [esp+0Ch]
                0xE8                                     // call rel32 (we check opcode only)
            };
            if (memcmp(p, expectedPrologue, 5) != 0)
            {
                _MESSAGE("MediumArmor: Calc_ArmorRating prologue bytes: "
                    "%02X %02X %02X %02X %02X %02X %02X %02X %02X %02X",
                    p[0], p[1], p[2], p[3], p[4], p[5], p[6], p[7], p[8], p[9]);
                _ERROR("MediumArmor: Calc_ArmorRating prologue mismatch. Skipping hook 4.");
                goto skip_hook4;
            }

            s_stolenBytes_CalcAR = 9;  // fld (4) + call rel32 (5)

            // Resolve the absolute target of the call at +4
            UInt32 callSite = Addr::Calc_ArmorRating + 4;
            SInt32 origRelOffset = *(SInt32*)(callSite + 1);
            UInt32 callTarget = callSite + 5 + origRelOffset;  // absolute address of Calc_LuckModifiedSkill

            _MESSAGE("MediumArmor: Calc_ArmorRating call target (LuckModifiedSkill) = %08X", callTarget);

            // ── Build trampoline ───────────────────────────────────────────────
            //    [0..3]  fld dword ptr [esp+0Ch]    (copied verbatim)
            //    [4..8]  call <fixed-up rel32>      (recalculated for trampoline addr)
            //    [9..13] jmp Calc_ArmorRating+9     (resume original body)
            UInt32 trampSize = 9 + 5;  // stolen bytes + JMP rel32
            UInt8* tramp = static_cast<UInt8*>(
                VirtualAlloc(nullptr, trampSize, MEM_COMMIT | MEM_RESERVE,
                    PAGE_EXECUTE_READWRITE));
            if (!tramp)
            {
                _ERROR("MediumArmor: VirtualAlloc failed for Calc_ArmorRating trampoline.");
                goto skip_hook4;
            }

            // Copy the fld instruction verbatim (4 bytes, no relocation needed)
            memcpy(tramp, p, 4);

            // Write the call with a fixed-up relative offset
            tramp[4] = 0xE8;
            UInt32 callInTramp = reinterpret_cast<UInt32>(tramp) + 4;
            *(SInt32*)(tramp + 5) = static_cast<SInt32>(callTarget - (callInTramp + 5));

            // Write JMP back to Calc_ArmorRating+9
            UInt32 resumeTarget = Addr::Calc_ArmorRating + 9;
            tramp[9] = 0xE9;
            UInt32 jmpInTramp = reinterpret_cast<UInt32>(tramp) + 9;
            *(SInt32*)(tramp + 10) = static_cast<SInt32>(resumeTarget - (jmpInTramp + 5));

            s_resumeAddr_CalcAR = reinterpret_cast<UInt32>(tramp);

            // Save original bytes for unhooking
            memcpy(s_origBytes_CalcAR, p, s_stolenBytes_CalcAR);

            // Write JMP to our detour (5 bytes) + NOP remaining 4
            WriteRelJump(Addr::Calc_ArmorRating,
                reinterpret_cast<UInt32>(&Detour_CalcArmorRating));
            for (UInt32 i = 5; i < s_stolenBytes_CalcAR; ++i)
                SafeWrite8(Addr::Calc_ArmorRating + i, 0x90);

            hook4_ok = true;
            _MESSAGE("MediumArmor: Hook 4 (Calc_ArmorRating) installed. "
                "Trampoline at %08X, LuckModSkill at %08X, resume at %08X.",
                reinterpret_cast<UInt32>(tramp), callTarget, resumeTarget);
        }
    skip_hook4:

        // ════════════════════════════════════════════════════════════════════════
        //  Hook 3: GetArmorSkillAV  (only if hook 4 is active — they're paired)
        // ════════════════════════════════════════════════════════════════════════
        if (hook4_ok)
        {
            const UInt8 expected[] = {
                0x8A, 0x41, 0x6A,
                0x24, 0x80,
                0xF6, 0xD8,
                0x1B, 0xC0,
                0x83, 0xE0, 0xF7,
                0x83, 0xC0, 0x1B,
                0xC3
            };
            UInt8* p = reinterpret_cast<UInt8*>(Addr::GetArmorSkillAV);
            _MESSAGE("MediumArmor: GetArmorSkillAV bytes: "
                "%02X %02X %02X %02X %02X %02X %02X %02X "
                "%02X %02X %02X %02X %02X %02X %02X %02X",
                p[0], p[1], p[2], p[3], p[4], p[5], p[6], p[7],
                p[8], p[9], p[10], p[11], p[12], p[13], p[14], p[15]);

            if (memcmp(p, expected, kStolenBytes_SkillAV) != 0)
            {
                _ERROR("MediumArmor: GetArmorSkillAV mismatch — skipping hook 3.");
            }
            else
            {
                memcpy(s_origBytes_SkillAV, p, kStolenBytes_SkillAV);
                WriteRelJump(Addr::GetArmorSkillAV,
                    reinterpret_cast<UInt32>(&Detour_GetArmorSkillAV));
                for (UInt32 i = 5; i < kStolenBytes_SkillAV; ++i)
                    SafeWrite8(Addr::GetArmorSkillAV + i, 0x90);
                _MESSAGE("MediumArmor: Hook 3 (GetArmorSkillAV) installed.");
            }
        }
        else
        {
            _WARNING("MediumArmor: Hook 4 failed — skipping hook 3 (flag pair). "
                "Inventory AR display will use light/heavy skill for medium armor.");
        }

        s_hookInstalled = true;
        _MESSAGE("MediumArmor: All hooks installed.");
        return true;
    }

    void RemoveHooks()
    {
        if (!s_hookInstalled)
            return;

        SafeWriteBuf(Addr::Sub_488CB0, s_origBytes_488CB0, kStolenBytes_488CB0);
        SafeWriteBuf(Addr::IsHeavyArmor, s_origBytes_IHA, kStolenBytes_IsHeavyArmor);
        SafeWriteBuf(Addr::GetArmorSkillAV, s_origBytes_SkillAV, kStolenBytes_SkillAV);
        if (s_stolenBytes_CalcAR > 0)
            SafeWriteBuf(Addr::Calc_ArmorRating, s_origBytes_CalcAR, s_stolenBytes_CalcAR);

        s_hookInstalled = false;
        _MESSAGE("MediumArmor: All hooks removed.");
    }

}