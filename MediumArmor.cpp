// ============================================================================
//  MediumArmor OBSE Plugin – MediumArmor.cpp
// ============================================================================

#include "MediumArmor.h"
#include "Config.h"

#include "OBSEKeywords/KeywordAPI.h"

#include "obse/GameAPI.h"
#include "obse/GameData.h"
#include "obse/GameForms.h"
#include "obse/GameObjects.h"
#include "obse/GameExtraData.h"

#include <algorithm>
#include <cmath>
#include <obse/ModTable.h>

namespace MediumArmor
{

    // ── Internal state ─────────────────────────────────────────────────────────
    TESGlobal* g_mediumArmorGlobal;
    TESGlobal* g_mediumArmorXPGlobal;
    static float s_mediumArmorSkill = 5.0f;   // default starting value

    // ════════════════════════════════════════════════════════════════════════════
    //  Keyword interface
    // ════════════════════════════════════════════════════════════════════════════

    bool HasKeyword(TESForm* form, const char* keyword)
    {
        // ┌──────────────────────────────────────────────────────────────────────┐
        // │  STUB — Replace this with your Keyword mod's actual API.            │
        // │                                                                     │
        // │  Option A – If your keyword plugin exposes a C++ function:          │
        // │      return YourKeywordLib::FormHasKeyword(form, keyword);          │
        // │                                                                     │
        // │  Option B – If it works via OBSE inter-plugin messaging:            │
        // │      Send a message through OBSEMessagingInterface and cache the    │
        // │      reply.  See main.cpp MessageHandler for the dispatch pattern.  │
        // │                                                                     │
        // │  Option C – Lightweight fallback: check the form's EditorID for a   │
        // │      substring.  Shown below as a temporary stand-in.               │
        // └──────────────────────────────────────────────────────────────────────┘

        if (!form || !keyword)
            return false;

        return KeywordAPI::HasKeyword(form->refID, keyword);
    }

    bool IsMediumArmor(TESForm* form)
    {
        if (!form)
            return false;

        // Only armour forms can be medium armour.
        if (form->typeID != kFormType_Armor)
            return false;

        return HasKeyword(form, kMediumArmorKeyword);
    }

    // ════════════════════════════════════════════════════════════════════════════
    //  Skill cache
    // ════════════════════════════════════════════════════════════════════════════

    float GetMediumArmorSkill()
    {
        return s_mediumArmorSkill;
    }

    void SetMediumArmorSkill(float value)
    {
        s_mediumArmorSkill = std::clamp(value, 0.0f, 100.0f);
    }

    void SyncSkillFromMenuQue()
    {
        UInt8 modIndex = ModTable::Get().GetModIndex("GPMediumArmor.esp");
        if (modIndex == 0xFF)
            return;

        UInt32 fullID = (modIndex << 24) | 0xED8;
        TESForm* globalForm =  LookupFormByID(fullID);
        g_mediumArmorGlobal = OBLIVION_CAST(globalForm, TESForm, TESGlobal);
        if (!g_mediumArmorGlobal) return;
        s_mediumArmorSkill = g_mediumArmorGlobal->data;
    }

    // ════════════════════════════════════════════════════════════════════════════
    //  XP helpers
    // ════════════════════════════════════════════════════════════════════════════

    float CalculateXPGain(float currentSkill)
    {
        // Diminishing returns: higher skill = less XP per hit.
        // XP = kXPPerHit * (1 - kXPSkillFactor * skill/100)
        float factor = 1.0f - kXPSkillFactor * (currentSkill / 100.0f);
        return kXPPerHit * std::max(factor, 0.05f);  // floor at 5 % of base
    }

    void AwardXP(float xp)
    {
        UInt8 modIndex = ModTable::Get().GetModIndex("GPMediumArmor.esp");
        if (modIndex == 0xFF)
            return;

        UInt32 fullID = (modIndex << 24) | 0x4578;
        TESForm* globalForm = LookupFormByID(fullID);
        g_mediumArmorXPGlobal = OBLIVION_CAST(globalForm, TESForm, TESGlobal);
        if (g_mediumArmorXPGlobal)
        {
            g_mediumArmorXPGlobal->data += xp;
        }
        SetMediumArmorSkill(GetMediumArmorSkill() + xp);
    }

    // ════════════════════════════════════════════════════════════════════════════
    //  Equipped-set helpers
    // ════════════════════════════════════════════════════════════════════════════

    /// Check whether an EntryData has any ExtraDataList with kExtraData_Worn
    /// or kExtraData_WornLeft, which is how Oblivion marks items as equipped.
    /// Shields and left-hand items use WornLeft.
    static bool IsEntryEquipped(ExtraContainerChanges::EntryData* entry)
    {
        if (!entry || !entry->extendData)
            return false;

        for (auto iter = entry->extendData->Begin(); !iter.End(); ++iter)
        {
            ExtraDataList* xList = iter.Get();
            if (xList && (xList->HasType(kExtraData_Worn) ||
                xList->HasType(kExtraData_WornLeft)))
                return true;
        }

        return false;
    }

    int CountEquippedMediumArmor(Actor* actor)
    {
        if (!actor)
            return 0;

        int count = 0;

        ExtraContainerChanges* xChanges = static_cast<ExtraContainerChanges*>(
            actor->baseExtraList.GetByType(kExtraData_ContainerChanges));

        if (!xChanges || !xChanges->data || !xChanges->data->objList)
            return 0;

        for (auto iter = xChanges->data->objList->Begin(); !iter.End(); ++iter)
        {
            ExtraContainerChanges::EntryData* entry = iter.Get();
            if (!entry || !entry->type)
                continue;

            if (!IsEntryEquipped(entry))
                continue;

            if (IsMediumArmor(entry->type))
                ++count;
        }

        return count;
    }

    bool IsWearingMediumArmor(Actor* actor)
    {
        return CountEquippedMediumArmor(actor) > 0;
    }

    bool IsWearingFullMediumSet(Actor* actor)
    {
        return CountEquippedMediumArmor(actor) >= kFullMediumSetCount;
    }

    int GetMediumArmorMasteryLevel()
    {
        float skill = GetMediumArmorSkill();
        if (skill >= 100.0f) return 4;  // Master
        if (skill >= 75.0f)  return 3;  // Expert
        if (skill >= 50.0f)  return 2;  // Journeyman
        if (skill >= 25.0f)  return 1;  // Apprentice
        return 0;                        // Novice
    }

}  // namespace MediumArmorPlugin