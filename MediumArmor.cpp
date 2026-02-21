// ============================================================================
//  MediumArmor OBSE Plugin – MediumArmor.cpp
// ============================================================================

#include "MediumArmor.h"
#include "Config.h"

#include "obse/GameAPI.h"
#include "obse/GameForms.h"
#include "obse/GameObjects.h"

#include "OBSEKeywords/KeywordAPI.h"

#include <algorithm>
#include <cmath>

namespace MediumArmor
{

    static float s_mediumArmorSkill = 5.0f;

    bool HasKeyword(TESForm* form, const char* keyword)
    {
        if (KeywordAPI::HasKeyword(form->refID, keyword))
        {
            return true;
        }

        if (!form || !keyword)
            return false;

        const char* editorID = form->GetEditorID();
        if (editorID && strstr(editorID, keyword))
            return true;

        return false;
    }

    bool IsMediumArmor(TESForm* form)
    {
        if (!form)
            return false;

        if (form->typeID != kFormType_Armor)
            return false;

        return HasKeyword(form, kMediumArmorKeyword);
    }

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
        // TODO
    }

    float CalculateXPGain(float currentSkill)
    {
        float factor = 1.0f - kXPSkillFactor * (currentSkill / 100.0f);
        return kXPPerHit * std::max(factor, 0.05f);
    }

    void AwardXP(float xp)
    {
        // TODO
        SetMediumArmorSkill(GetMediumArmorSkill() + xp);
    }

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

}