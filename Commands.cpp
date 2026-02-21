#include "Commands.h"
#include "MediumArmor.h"
#include "Config.h"

#include "obse/GameAPI.h"
#include "obse/GameForms.h"
#include "obse/GameObjects.h"
#include "obse/ParamInfos.h"
#include "obse/CommandTable.h"

namespace MediumArmor
{
    static ParamInfo kParams_OneFloat[] =
    {
        { "float", kParamType_Float, 0 },
    };

    static ParamInfo kParams_OneRef[] =
    {
        { "ref", kParamType_InventoryObject, 1 },  // optional calling ref
    };

    static ParamInfo kParams_OneActorRef[] =
    {
        { "actorRef", kParamType_Actor, 1 },
    };


    static bool Cmd_GetMediumArmorSkill_Execute(COMMAND_ARGS)
    {
        *result = static_cast<double>(GetMediumArmorSkill());
        if (IsConsoleMode())
            Console_Print("GetMediumArmorSkill >> %.2f", *result);
        return true;
    }

    static bool Cmd_SetMediumArmorSkill_Execute(COMMAND_ARGS)
    {
        float value = 0.0f;
        if (!ExtractArgs(PASS_EXTRACT_ARGS, &value))
            return true;

        SetMediumArmorSkill(value);

        if (IsConsoleMode())
            Console_Print("SetMediumArmorSkill >> %.2f", value);
        return true;
    }

    static bool Cmd_ModMediumArmorSkill_Execute(COMMAND_ARGS)
    {
        float delta = 0.0f;
        if (!ExtractArgs(PASS_EXTRACT_ARGS, &delta))
            return true;

        float newVal = GetMediumArmorSkill() + delta;
        SetMediumArmorSkill(newVal);

        if (IsConsoleMode())
            Console_Print("ModMediumArmorSkill >> %.2f (delta %.2f)", GetMediumArmorSkill(), delta);
        return true;
    }

    static bool Cmd_IsMediumArmor_Execute(COMMAND_ARGS)
    {
        *result = 0.0;
        TESForm* form = nullptr;

        if (!ExtractArgs(PASS_EXTRACT_ARGS, &form))
            return true;

        // If no explicit arg, use the calling reference.
        if (!form && thisObj)
            form = thisObj->baseForm;

        if (form && IsMediumArmor(form))
            *result = 1.0;

        if (IsConsoleMode())
            Console_Print("IsMediumArmor >> %d", static_cast<int>(*result));
        return true;
    }

    static bool Cmd_GetEquippedMediumCount_Execute(COMMAND_ARGS)
    {
        *result = 0.0;
        Actor* actor = nullptr;

        if (!ExtractArgs(PASS_EXTRACT_ARGS, &actor))
            actor = nullptr;

        if (!actor)
        {
            if (thisObj && thisObj->IsActor())
                actor = static_cast<Actor*>(thisObj);
            else
                actor = *g_thePlayer;
        }

        if (actor)
            *result = static_cast<double>(CountEquippedMediumArmor(actor));

        if (IsConsoleMode())
            Console_Print("GetEquippedMediumCount >> %d", static_cast<int>(*result));
        return true;
    }

    static bool Cmd_IsWearingMediumArmor_Execute(COMMAND_ARGS)
    {
        *result = 0.0;
        Actor* actor = nullptr;

        if (!ExtractArgs(PASS_EXTRACT_ARGS, &actor))
            actor = nullptr;

        if (!actor)
        {
            if (thisObj && thisObj->IsActor())
                actor = static_cast<Actor*>(thisObj);
            else
                actor = *g_thePlayer;
        }

        if (actor && IsWearingMediumArmor(actor))
            *result = 1.0;

        if (IsConsoleMode())
            Console_Print("IsWearingMediumArmor >> %d", static_cast<int>(*result));
        return true;
    }

    CommandInfo kCommandInfo_GetMediumArmorSkill =
    {
        "GetMediumArmorSkill",
        "GetMedSkill",
        kCmd_GetMediumArmorSkill,
        "Returns the player's medium-armour skill (0-100).",
        0,              // requires parent obj? no
        0,              // num params
        nullptr,        // param info
        HANDLER(Cmd_GetMediumArmorSkill_Execute)
    };

    CommandInfo kCommandInfo_SetMediumArmorSkill =
    {
        "SetMediumArmorSkill",
        "SetMedSkill",
        kCmd_SetMediumArmorSkill,
        "Sets the player's medium-armour skill to the given value.",
        0,
        1,
        kParams_OneFloat,
        HANDLER(Cmd_SetMediumArmorSkill_Execute)
    };

    CommandInfo kCommandInfo_ModMediumArmorSkill =
    {
        "ModMediumArmorSkill",
        "ModMedSkill",
        kCmd_ModMediumArmorSkill,
        "Adds a delta to the player's medium-armour skill.",
        0,
        1,
        kParams_OneFloat,
        HANDLER(Cmd_ModMediumArmorSkill_Execute)
    };

    CommandInfo kCommandInfo_IsMediumArmor =
    {
        "IsMediumArmor",
        "",
        kCmd_IsMediumArmor,
        "Returns 1 if the form/reference is classified as medium armour.",
        0,
        1,
        kParams_OneRef,
        HANDLER(Cmd_IsMediumArmor_Execute)
    };

    CommandInfo kCommandInfo_GetEquippedMediumCount =
    {
        "GetEquippedMediumCount",
        "GetMedCount",
        kCmd_GetEquippedMediumCount,
        "Returns the number of medium-armour pieces the actor has equipped.",
        0,
        1,
        kParams_OneActorRef,
        HANDLER(Cmd_GetEquippedMediumCount_Execute)
    };

    CommandInfo kCommandInfo_IsWearingMediumArmor =
    {
        "IsWearingMediumArmor",
        "WearMedArmor",
        kCmd_IsWearingMediumArmor,
        "Returns 1 if the actor has at least one medium-armour piece equipped.",
        0,
        1,
        kParams_OneActorRef,
        HANDLER(Cmd_IsWearingMediumArmor_Execute)
    };

}