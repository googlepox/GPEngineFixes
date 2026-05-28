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
#include <EditorIDMapper/EditorIDMapperAPI.h>

#include <boost/algorithm/string.hpp>

namespace MediumArmor
{

    // ── Internal state ─────────────────────────────────────────────────────────
    TESGlobal* g_mediumArmorGlobal;
    TESGlobal* g_mediumArmorXPGlobal;
    static float MA_SKILL_START = 5.0f;
    std::vector<std::string> MA_MATERIALS = { "medium", "mithril", "dragonscale", 
        "bonemold", "orcish", "chainmail", "ordinator", "dreugh", "maomer", 
        "maormer", "leatherhard", "brass", "bronze", "adamantium", "jade",
        "indoril", "trollbone", "netchad"};

    std::unordered_map<std::string, float*> configMap =
    {
        { "MA_SKILL_START",   &MA_SKILL_START },
    };

    // ════════════════════════════════════════════════════════════════════════════
    //  Keyword interface
    // ════════════════════════════════════════════════════════════════════════════

    bool HasKeyword(TESForm* form, const char* keyword)
    {
        if (!form || !keyword)
            return false;

        return KeywordAPI::HasKeyword(form->refID, keyword);
    }

    bool IsMediumArmorMaterial(TESForm* form)
    {
        std::string editorID = EditorIDMapper::ReverseLookup(form->refID);

        if (editorID.empty())
            editorID = form->GetEditorName();

        if (editorID.empty())
            return false;

        boost::algorithm::to_lower(editorID);

        for (const auto& material : MA_MATERIALS)
        {
            if (editorID.contains(material))
                return true;
        }

        return false;
    }

    bool IsMediumArmor(TESForm* form)
    {
        if (!form)
            return false;

        if (form->typeID != kFormType_Armor)
            return false;

        bool hasKeyword = HasKeyword(form, kMediumArmorKeyword);
        
        if (!hasKeyword)
        {
            return IsMediumArmorMaterial(form);
        }

        return hasKeyword;
    }

    // ════════════════════════════════════════════════════════════════════════════
    //  Skill cache
    // ════════════════════════════════════════════════════════════════════════════

    float GetMediumArmorSkill()
    {
        return MA_SKILL_START;
    }

    void SetMediumArmorSkill(float value)
    {
        MA_SKILL_START = std::clamp(value, 0.0f, 100.0f);
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
        MA_SKILL_START = g_mediumArmorGlobal->data;
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

    void LoadConfig()
    {
        std::ifstream file("Data\\OBSE\\Plugins\\MediumArmor\\Config.ini");
        if (!file.is_open())
        {
            _MESSAGE("Config.ini not found");
            return;
        }

        std::string line;
        enum Section { NONE, CONFIG, MATERIALS } section = NONE;

        while (std::getline(file, line))
        {
            if (line.empty() || line[0] == ';')
                continue;

            if (line[0] == '[')
            {
                if (line == "[Config]") section = CONFIG;
                else if (line == "[Materials]") section = MATERIALS;
                else section = NONE;
                continue;
            }

            std::stringstream ss(line);

            if (section == CONFIG)
            {
                std::string key, value;

                std::getline(ss, key, ',');
                std::getline(ss, value);

                float f = std::stof(value);

                auto it = configMap.find(key);
                if (it != configMap.end())
                {
                    *it->second = f;
                }
            }
            else if (section == MATERIALS)
            {
                MA_MATERIALS.clear();

                std::string material;

                while (std::getline(ss, material, ','))
                {
                    boost::algorithm::trim(material);
                    boost::algorithm::to_lower(material);

                    if (!material.empty())
                    {
                        MA_MATERIALS.push_back(material);
                    }
                }
            }
        }
    }

}  // namespace MediumArmorPlugin