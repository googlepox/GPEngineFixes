#pragma once
// ============================================================================
//  MediumArmor OBSE Plugin – Commands.h
//  Declares new OBSE script commands available to modders.
// ============================================================================

#include "obse/CommandTable.h"
#include "obse/ParamInfos.h"
#include <obse/PluginAPI.h>

namespace MediumArmor
{

    constexpr UInt32 kCmdBase = 0x2800;

    enum CommandOpcodes : UInt32
    {
        kCmd_GetMediumArmorSkill = kCmdBase + 0,
        kCmd_SetMediumArmorSkill = kCmdBase + 1,
        kCmd_ModMediumArmorSkill = kCmdBase + 2,
        kCmd_IsMediumArmor = kCmdBase + 3,
        kCmd_GetEquippedMediumCount = kCmdBase + 4,
        kCmd_IsWearingMediumArmor = kCmdBase + 5,
    };

    extern CommandInfo kCommandInfo_GetMediumArmorSkill;
    extern CommandInfo kCommandInfo_SetMediumArmorSkill;
    extern CommandInfo kCommandInfo_ModMediumArmorSkill;
    extern CommandInfo kCommandInfo_IsMediumArmor;
    extern CommandInfo kCommandInfo_GetEquippedMediumCount;
    extern CommandInfo kCommandInfo_IsWearingMediumArmor;

}  // namespace MediumArmor