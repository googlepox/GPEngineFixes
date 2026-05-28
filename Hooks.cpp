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

        // Hook 5: encumbrance — mid-function in GetEncumbrance, light armor path
        //   488057: 8B CD              mov ecx, ebp          ; 2 bytes
        //   488059: E8 xx xx xx xx     call IsHeavyArmor     ; 5 bytes
        //   48805E: ...                ← resume here (test al, al)
        constexpr UInt32 Encumbrance_Hook = 0x00488057;  // 7 bytes
        constexpr UInt32 Encumbrance_Resume = 0x0048805E;  // after stolen bytes (test al,al)
        constexpr UInt32 Encumbrance_Skip = 0x00488086;  // past all perk logic

        // Hook 6: degradation — mid-function in DamageEquippedItem, light path
        //   5F38EE: 8B CD              mov ecx, ebp          ; 2 bytes
        //   5F38F0: E8 xx xx xx xx     call IsHeavyArmor     ; 5 bytes
        //   5F38F5: ...                ← resume here (test al, al)
        constexpr UInt32 Degradation_Hook = 0x005F38EE;  // 7 bytes
        constexpr UInt32 Degradation_Resume = 0x005F38F5;  // vanilla light path continues
        constexpr UInt32 Degradation_Skip = 0x005F392F;  // LABEL_17 (past all perk logic)

        // Hook 7: XP award — in Actor_AttackHandling, after armor hit
        //   5FFC81: E8 xx xx xx xx     call IsHeavyArmor     ; 5 bytes exactly
        //   5FFC86: D9 EE              fldz                   ; ← resume here
        //   5FFCA4: ...                ← skip here (past entire award block)
        constexpr UInt32 SkillXP_Hook = 0x005FFC81;  // 5 bytes
        constexpr UInt32 SkillXP_Resume = 0x005FFC86;  // fldz (vanilla continues)
        constexpr UInt32 SkillXP_Skip = 0x005FFCA4;  // past AwardSkillUsage call

        // Hook 8: spell effectiveness — replace call to Calc_ArmorSpellEffectiveness
        //   65DBCA: E8 xx xx xx xx     call Calc_ArmorSpellEffectiveness  ; 5 bytes
        //   Target address extracted at init from the call's rel32 operand.
        constexpr UInt32 SpellEff_Hook = 0x0065DBCA;  // 5 bytes

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
    constexpr UInt32 kStolenBytes_Encumbrance = 7;   // mov ecx,ebp + call IsHeavyArmor
    constexpr UInt32 kStolenBytes_Degradation = 7;   // mov ecx,ebp + call IsHeavyArmor
    constexpr UInt32 kStolenBytes_SkillXP = 5;   // call IsHeavyArmor (exactly 5)
    constexpr UInt32 kStolenBytes_SpellEff = 5;   // call Calc_ArmorSpellEffectiveness
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
    static UInt8 s_origBytes_Encumbrance[kStolenBytes_Encumbrance];
    static UInt8 s_origBytes_Degradation[kStolenBytes_Degradation];
    static UInt8 s_origBytes_SkillXP[kStolenBytes_SkillXP];
    static UInt8 s_origBytes_SpellEff[kStolenBytes_SpellEff];
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

        // ── Master perk: +25% AR when wearing a full medium set ────────────────
        if (GetMediumArmorMasteryLevel() >= 4)
        {
            Actor* actorPtr = reinterpret_cast<Actor*>(actor);
            if (IsWearingFullMediumSet(actorPtr))
                result *= kPerkMasterARBonus;
        }

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

}  // close MediumArmorPlugin namespace temporarily

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
    MediumArmor::SyncSkillFromMenuQue();
    float skill = MediumArmor::GetMediumArmorSkill()
        * MediumArmor::kARMultiplier
        + MediumArmor::kARFlat;
    _MESSAGE("Medium Armor skill is %f", skill);
    return std::clamp(skill, 0.0f, 255.0f);
}
static float(__cdecl* s_fnGetMediumSkill)() = &GetMediumSkillWithMods;

// ── Encumbrance hook (Hook 5) globals ──────────────────────────────────────
static UInt32 s_resumeAddr_Enc = 0;   // → 0x48805E (test al, al — vanilla light path)
static UInt32 s_skipAddr_Enc = 0;   // → 0x488086 (past all perk logic)
static UInt32 s_addrIsHeavyArmor = 0; // → 0x4B4C70 (for indirect call in stolen bytes)

// Static float constants for FPU multiply in naked ASM
static float s_expertEncumMult = MediumArmor::kPerkExpertEncumbranceMult;  // 0.5
static float s_zeroFloat = 0.0f;

// ── Degradation hook (Hook 6) globals ──────────────────────────────────────
static UInt32 s_resumeAddr_Deg = 0;   // → 0x5F38F5 (vanilla light path continues)
static UInt32 s_skipAddr_Deg = 0;   // → 0x5F392F (LABEL_17, past all perk logic)

// Static float constants for degradation perk multipliers
static float s_noviceDamageMult = MediumArmor::kPerkNoviceDamageMult;      // 1.5
static float s_journeymanDamageMult = MediumArmor::kPerkJourneymanDamageMult;  // 0.5

// ── Skill XP hook (Hook 7) globals ─────────────────────────────────────────
static UInt32 s_resumeAddr_XP = 0;    // → 0x5FFC86 (vanilla continues)
static UInt32 s_skipAddr_XP = 0;    // → 0x5FFCA4 (past AwardSkillUsage)

// Helper: award medium armor XP for a combat hit
static void __cdecl AwardMediumArmorXP()
{
    MediumArmor::SyncSkillFromMenuQue();
    float skill = MediumArmor::GetMediumArmorSkill();
    float xp = MediumArmor::CalculateXPGain(skill);
    MediumArmor::AwardXP(xp);
}
static void(__cdecl* s_fnAwardMediumXP)() = &AwardMediumArmorXP;

// ── Spell effectiveness hook (Hook 8) globals ───────────────────────────────
// Original Calc_ArmorSpellEffectiveness — extracted from call site at init
typedef double(__cdecl* fnCalcSpellEff)(int, int, int, int);
static fnCalcSpellEff s_fnCalcSpellEff = nullptr;

// Actor captured by naked thunk so C++ function can access it
static Actor* s_actorForSpellCalc = nullptr;

// Game setting floats (game settings loaded into these addresses at startup)
static const float* s_fMagicPenaltyMin = reinterpret_cast<const float*>(0xB37F50);
static const float* s_fMagicPenaltyMax = reinterpret_cast<const float*>(0xB37F58);

// Per-slot coverage game settings (ints, already loaded at startup)
static const int* s_iHelmChance = reinterpret_cast<const int*>(0xB36EC8);
static const int* s_iCuirassChance = reinterpret_cast<const int*>(0xB36EB8);
static const int* s_iGreavesChance = reinterpret_cast<const int*>(0xB36EC0);
static const int* s_iGauntletsChance = reinterpret_cast<const int*>(0xB36ED0);
static const int* s_iBootsChance = reinterpret_cast<const int*>(0xB36ED8);
static const int* s_iShieldChance = reinterpret_cast<const int*>(0xB36EE0);

// Computes medium-armor-only coverage using partMask (bipedModel at armo+0x64, partMask at +0x04)
static int __cdecl GetMediumArmorCoverage(Actor* actor)
{
    if (!actor) return 0;

    int coverage = 0;

    ExtraContainerChanges* xChanges = static_cast<ExtraContainerChanges*>(
        actor->baseExtraList.GetByType(kExtraData_ContainerChanges));

    if (!xChanges || !xChanges->data || !xChanges->data->objList)
        return 0;

    for (auto iter = xChanges->data->objList->Begin(); !iter.End(); ++iter)
    {
        ExtraContainerChanges::EntryData* entry = iter.Get();
        if (!entry || !entry->type)
            continue;

        // Only equipped medium armor pieces
        bool worn = false;
        if (entry->extendData)
        {
            for (auto xIter = entry->extendData->Begin(); !xIter.End(); ++xIter)
            {
                ExtraDataList* xList = xIter.Get();
                if (xList && (xList->HasType(kExtraData_Worn) ||
                    xList->HasType(kExtraData_WornLeft)))
                {
                    worn = true;
                    break;
                }
            }
        }
        if (!worn) continue;
        if (!MediumArmor::IsMediumArmor(entry->type)) continue;

        // Read partMask: bipedModel at armo+0x64, partMask at +0x04 → armo+0x68
        UInt32 partMask = *reinterpret_cast<const UInt32*>(
            reinterpret_cast<const UInt8*>(entry->type) + 0x68);

        if (partMask & (1 << 0))  coverage += *s_iHelmChance;      // Head
        if (partMask & (1 << 1))  coverage += *s_iHelmChance;      // Hair (same chance)
        if (partMask & (1 << 2))  coverage += *s_iCuirassChance;   // UpperBody
        if (partMask & (1 << 3))  coverage += *s_iGreavesChance;   // LowerBody
        if (partMask & (1 << 4))  coverage += *s_iGauntletsChance; // Hand
        if (partMask & (1 << 5))  coverage += *s_iBootsChance;     // Foot
        if (partMask & (1 << 13)) coverage += *s_iShieldChance;    // Shield
    }

    return std::clamp(coverage, 0, 100);
}

// Full three-category spell effectiveness formula
static double __cdecl Calc_SpellEffectivenessWithMedium(
    int lightSkill, int lightCoverage, int heavySkill, int heavyCoverage)
{
    Actor* actor = s_actorForSpellCalc;

    int medCoverage = GetMediumArmorCoverage(actor);
    int adjLightCoverage = std::max(0, lightCoverage - medCoverage);

    // Call vanilla with corrected light coverage
    double result = s_fnCalcSpellEff(lightSkill, adjLightCoverage, heavySkill, heavyCoverage);
    // Add medium penalty term using same formula as vanilla
    if (medCoverage > 0)
    {
        float penaltyMin = *s_fMagicPenaltyMin;
        float penaltyMax = *s_fMagicPenaltyMax;
        float medSkill = MediumArmor::GetMediumArmorSkill();

        // Penalty = 0 at skill >= 100; scales up as skill drops toward 0
        double medPenalty = 0.0;
        int skillCapped = std::min(static_cast<int>(medSkill), 100);
        medPenalty = (medCoverage / 100.0) * (2 * (50 - skillCapped));

        result -= static_cast<double>(penaltyMax - penaltyMin) * medPenalty / 100.0;
    }
    return std::clamp(result, 0.0, 1.0);
}

// Helper: returns medium mastery level as int
static int __cdecl GetMediumMasteryLevel()
{
    MediumArmor::SyncSkillFromMenuQue();
    return MediumArmor::GetMediumArmorMasteryLevel();
}
static int(__cdecl* s_fnGetMediumMastery)() = &GetMediumMasteryLevel;

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
//  Hook 5 — Encumbrance mid-function detour  (0x00488057)
//  ────────────────────────────────────────────────────────────────────────
//  Replaces the redundant IsHeavyArmor call in GetEncumbrance's light path.
//
//  Stolen: mov ecx, ebp (2) + call IsHeavyArmor (5) = 7 bytes
//
//  Register contract at entry:
//    EBP = TESObjectARMO*  (the armor piece being evaluated)
//    Stack layout (depth 0x24):
//      [ESP+0x10] = var_14 = base weight (float)
//      [ESP+0x1C] = var_8  = output weight (float, already = base weight)
//      [ESP+0x18] = var_C  = actor pointer (or 0)
//
//  Medium → apply our encumbrance perk → JMP to 0x488086 (skip light perk)
//  Not medium → stolen bytes → JMP to 0x48805E (vanilla light perk path)
// ════════════════════════════════════════════════════════════════════════════

static __declspec(naked) void Detour_Encumbrance()
{
    __asm
    {
        // ── Check IsMediumArmor(ebp) ───────────────────────────────────────
        push    eax                         // save (flags/return val in progress)
        push    ebp                         // arg: TESObjectARMO*
        call[s_fnIsMediumArmor]         // bool __cdecl
        add     esp, 4
        test    al, al
        pop     eax                         // restore (flags unaffected by pop)
        jnz     medium_enc

        // ── Not medium: execute stolen bytes, resume vanilla light path ────
        mov     ecx, ebp                    // stolen byte 1
        call[s_addrIsHeavyArmor]        // stolen byte 2 (indirect thiscall)
        jmp[s_resumeAddr_Enc]          // → 0x48805E (test al, al)

        medium_enc:
        // ── Get mastery level ──────────────────────────────────────────────
        call[s_fnGetMediumMastery]      // eax = 0..4
            cmp     eax, 4
            jge     master_enc
            cmp     eax, 3
            jge     expert_enc

            // ── Below Expert: no encumbrance perk ──────────────────────────────
            jmp[s_skipAddr_Enc]            // → 0x488086

            expert_enc:
        fld     dword ptr[esp + 0x10]      // var_14 (base weight)
            fmul    dword ptr[s_expertEncumMult] // × 0.5
            fstp    dword ptr[esp + 0x1C]      // var_8 (output weight)
            jmp[s_skipAddr_Enc]

            master_enc :
            fld     dword ptr[s_zeroFloat]     // 0.0
            fstp    dword ptr[esp + 0x1C]      // var_8 = 0 (weightless)
            jmp[s_skipAddr_Enc]
    }
}

// ════════════════════════════════════════════════════════════════════════════
//  Hook 6 — Degradation mid-function detour  (0x005F38EE)
//  ────────────────────────────────────────────────────────────────────────
//  Replaces the redundant IsHeavyArmor call in DamageEquippedItem's light path.
//
//  Stolen: mov ecx, ebp (2) + call IsHeavyArmor (5) = 7 bytes
//
//  Register contract at entry (stack depth 0x24):
//    EBP = TESObjectARMO*  (the armor piece taking damage)
//    ESI = Actor*
//    Stack:
//      [ESP+0x28] = arg_0 slot = modified damage (float, output)
//      [ESP+0x2C] = arg_4 slot = original damage (float, input)
//
//  Medium → apply our degradation perk mult → JMP to LABEL_17 (0x5F392F)
//  Not medium → stolen bytes → JMP to 0x5F38F5 (vanilla light perk path)
// ════════════════════════════════════════════════════════════════════════════

static __declspec(naked) void Detour_Degradation()
{
    __asm
    {
        // ── Check IsMediumArmor(ebp) ───────────────────────────────────────
        push    eax
        push    ebp                         // arg: TESObjectARMO*
        call[s_fnIsMediumArmor]         // bool __cdecl
        add     esp, 4
        test    al, al
        pop     eax
        jnz     medium_deg

        // ── Not medium: execute stolen bytes, resume vanilla light path ────
        mov     ecx, ebp                    // stolen byte 1
        call[s_addrIsHeavyArmor]        // stolen byte 2 (indirect thiscall)
        jmp[s_resumeAddr_Deg]          // → 0x5F38F5 (test al, al)

        medium_deg:
        // ── Get mastery level ──────────────────────────────────────────────
        call[s_fnGetMediumMastery]      // eax = 0..4
            cmp     eax, 2
            jge     journeyman_deg
            test    eax, eax
            jz      novice_deg

            // ── Apprentice (mastery 1): no multiplier, damage unchanged ────────
            jmp[s_skipAddr_Deg]            // → LABEL_17

            novice_deg:
        // ── Novice: damage *= 1.5 ─────────────────────────────────────────
        fld     dword ptr[s_noviceDamageMult]
            fmul    dword ptr[esp + 0x2C]     // × original damage (arg_4)
            fstp    dword ptr[esp + 0x28]     // → modified damage (arg_0)
            jmp[s_skipAddr_Deg]

            journeyman_deg :
            // ── Journeyman+ (mastery 2+): damage *= 0.5 ──────────────────────
            fld     dword ptr[s_journeymanDamageMult]
            fmul    dword ptr[esp + 0x2C]     // × original damage
            fstp    dword ptr[esp + 0x28]     // → modified damage
            jmp[s_skipAddr_Deg]
    }
}

// ════════════════════════════════════════════════════════════════════════════
//  Hook 7 — Skill XP award detour  (0x005FFC81)
//  ────────────────────────────────────────────────────────────────────────
//  Replaces the IsHeavyArmor call in the armor-hit XP award path.
//
//  Stolen: call IsHeavyArmor = 5 bytes exactly
//
//  Register contract at entry:
//    ECX = TESObjectARMO*  (the armor piece that absorbed the hit)
//    ESI = Actor*  (the defender)
//
//  Medium → call AwardMediumArmorXP, skip vanilla AwardSkillUsage → 0x5FFCA4
//  Not medium → call IsHeavyArmor, resume → 0x5FFC86 (vanilla fldz)
// ════════════════════════════════════════════════════════════════════════════

static __declspec(naked) void Detour_SkillXP()
{
    __asm
    {
        // ECX = TESObjectARMO* (thiscall arg for IsHeavyArmor)
        push    eax
        push    ecx                         // save armor form
        push    ecx                         // arg: TESForm*
        call[s_fnIsMediumArmor]         // bool __cdecl
        add     esp, 4
        test    al, al
        pop     ecx                         // restore armor form
        pop     eax
        jnz     medium_xp

        // ── Not medium: call original IsHeavyArmor, resume vanilla ─────────
        call[s_addrIsHeavyArmor]        // thiscall, ECX already set
        jmp[s_resumeAddr_XP]           // → 0x5FFC86 (fldz)

        medium_xp:
        // ── Medium: award our XP, skip vanilla award block ─────────────────
        push    ecx
            push    edx
            call[s_fnAwardMediumXP]         // void __cdecl AwardMediumArmorXP()
            pop     edx
            pop     ecx
            jmp[s_skipAddr_XP]             // → 0x5FFCA4 (past AwardSkillUsage)
    }
}

// ════════════════════════════════════════════════════════════════════════════
//  Hook 8 — Spell effectiveness thunk  (0x0065DBCA)
//  ────────────────────────────────────────────────────────────────────────
//  Replaces the call to Calc_ArmorSpellEffectiveness.
//
//  At entry ESI = Actor*, stack = (lightSkill, lightCoverage,
//                                   heavySkill, heavyCoverage, luck)  cdecl
//
//  We capture ESI for the C++ function then tail-call it with the same args.
// ════════════════════════════════════════════════════════════════════════════

static __declspec(naked) void Thunk_SpellEffectiveness()
{
    __asm
    {
        mov     dword ptr[s_actorForSpellCalc], esi
        jmp     Calc_SpellEffectivenessWithMedium   // tail-call, same cdecl args
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

        // ════════════════════════════════════════════════════════════════════════
        //  Hook 5: Encumbrance (mid-function in GetEncumbrance)
        // ════════════════════════════════════════════════════════════════════════
        {
            // Initialize encumbrance hook addresses
            s_resumeAddr_Enc = Addr::Encumbrance_Resume;
            s_skipAddr_Enc = Addr::Encumbrance_Skip;
            s_addrIsHeavyArmor = Addr::IsHeavyArmor;  // calls our Hook 2 detour

            // Expected bytes: 8B CD E8 xx xx xx xx  (mov ecx,ebp + call rel32)
            UInt8* p = reinterpret_cast<UInt8*>(Addr::Encumbrance_Hook);
            _MESSAGE("MediumArmor: Encumbrance hook bytes: "
                "%02X %02X %02X %02X %02X %02X %02X",
                p[0], p[1], p[2], p[3], p[4], p[5], p[6]);

            if (p[0] != 0x8B || p[1] != 0xCD || p[2] != 0xE8)
            {
                _ERROR("MediumArmor: Encumbrance hook mismatch — skipping hook 5.");
            }
            else
            {
                memcpy(s_origBytes_Encumbrance, p, kStolenBytes_Encumbrance);
                WriteRelJump(Addr::Encumbrance_Hook,
                    reinterpret_cast<UInt32>(&Detour_Encumbrance));
                SafeWrite8(Addr::Encumbrance_Hook + 5, 0x90);
                SafeWrite8(Addr::Encumbrance_Hook + 6, 0x90);
                _MESSAGE("MediumArmor: Hook 5 (Encumbrance) installed.");
            }
        }

        // ════════════════════════════════════════════════════════════════════════
        //  Hook 6: Degradation (mid-function in DamageEquippedItem)
        // ════════════════════════════════════════════════════════════════════════
        {
            s_resumeAddr_Deg = Addr::Degradation_Resume;
            s_skipAddr_Deg = Addr::Degradation_Skip;

            UInt8* p = reinterpret_cast<UInt8*>(Addr::Degradation_Hook);
            _MESSAGE("MediumArmor: Degradation hook bytes: "
                "%02X %02X %02X %02X %02X %02X %02X",
                p[0], p[1], p[2], p[3], p[4], p[5], p[6]);

            if (p[0] != 0x8B || p[1] != 0xCD || p[2] != 0xE8)
            {
                _ERROR("MediumArmor: Degradation hook mismatch — skipping hook 6.");
            }
            else
            {
                memcpy(s_origBytes_Degradation, p, kStolenBytes_Degradation);
                WriteRelJump(Addr::Degradation_Hook,
                    reinterpret_cast<UInt32>(&Detour_Degradation));
                SafeWrite8(Addr::Degradation_Hook + 5, 0x90);
                SafeWrite8(Addr::Degradation_Hook + 6, 0x90);
                _MESSAGE("MediumArmor: Hook 6 (Degradation) installed.");
            }
        }

        // ════════════════════════════════════════════════════════════════════════
        //  Hook 7: Skill XP award (in Actor_AttackHandling)
        // ════════════════════════════════════════════════════════════════════════
        {
            s_resumeAddr_XP = Addr::SkillXP_Resume;
            s_skipAddr_XP = Addr::SkillXP_Skip;

            UInt8* p = reinterpret_cast<UInt8*>(Addr::SkillXP_Hook);
            _MESSAGE("MediumArmor: SkillXP hook bytes: %02X %02X %02X %02X %02X",
                p[0], p[1], p[2], p[3], p[4]);

            if (p[0] != 0xE8)
            {
                _ERROR("MediumArmor: SkillXP hook mismatch — expected E8 (call). Skipping hook 7.");
            }
            else
            {
                memcpy(s_origBytes_SkillXP, p, kStolenBytes_SkillXP);
                WriteRelJump(Addr::SkillXP_Hook,
                    reinterpret_cast<UInt32>(&Detour_SkillXP));
                _MESSAGE("MediumArmor: Hook 7 (SkillXP) installed.");
            }
        }

        // ════════════════════════════════════════════════════════════════════════
        //  Hook 8: Spell effectiveness (replace Calc_ArmorSpellEffectiveness call)
        // ════════════════════════════════════════════════════════════════════════
        {
            UInt8* p = reinterpret_cast<UInt8*>(Addr::SpellEff_Hook);
            _MESSAGE("MediumArmor: SpellEff hook bytes: %02X %02X %02X %02X %02X",
                p[0], p[1], p[2], p[3], p[4]);

            if (p[0] != 0xE8)
            {
                _ERROR("MediumArmor: SpellEff hook mismatch — expected E8 (call). Skipping hook 8.");
            }
            else
            {
                // Extract vanilla Calc_ArmorSpellEffectiveness address from the call's rel32
                SInt32 rel32 = *reinterpret_cast<SInt32*>(p + 1);
                s_fnCalcSpellEff = reinterpret_cast<fnCalcSpellEff>(
                    (Addr::SpellEff_Hook + 5) + static_cast<UInt32>(rel32));
                _MESSAGE("MediumArmor: Calc_ArmorSpellEffectiveness resolved to 0x%08X",
                    reinterpret_cast<UInt32>(s_fnCalcSpellEff));

                memcpy(s_origBytes_SpellEff, p, kStolenBytes_SpellEff);
                //WriteRelJump(Addr::SpellEff_Hook,
                    //reinterpret_cast<UInt32>(&Thunk_SpellEffectiveness));
                WriteRelCall(0x65DBCA,
                    reinterpret_cast<UInt32>(&Thunk_SpellEffectiveness));
                _MESSAGE("MediumArmor: Hook 8 (SpellEffectiveness) installed.");
            }
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
        SafeWriteBuf(Addr::Encumbrance_Hook, s_origBytes_Encumbrance, kStolenBytes_Encumbrance);
        SafeWriteBuf(Addr::Degradation_Hook, s_origBytes_Degradation, kStolenBytes_Degradation);
        SafeWriteBuf(Addr::SkillXP_Hook, s_origBytes_SkillXP, kStolenBytes_SkillXP);
        SafeWriteBuf(Addr::SpellEff_Hook, s_origBytes_SpellEff, kStolenBytes_SpellEff);

        s_hookInstalled = false;
        _MESSAGE("MediumArmor: All hooks removed.");
    }

}