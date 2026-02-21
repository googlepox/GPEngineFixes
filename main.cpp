#include "obse/PluginAPI.h"
#include "obse/CommandTable.h"

#include "OBSEKeywords/KeywordAPI.h"
#include "Hooks.h"

#if OBLIVION
#include "obse/GameAPI.h"

/*	As of 0020, ExtractArgsEx() and ExtractFormatStringArgs() are no longer directly included in plugin builds.
	They are available instead through the OBSEScriptInterface.
	To make it easier to update plugins to account for this, the following can be used.
	It requires that g_scriptInterface is assigned correctly when the plugin is first loaded.
*/

#else
#include "obse_editor/EditorAPI.h"
#endif

#include "obse/ParamInfos.h"
#include "obse/Script.h"
#include "obse/GameObjects.h"
#include <string>

IDebugLog		gLog("MediumArmor.log");

PluginHandle				g_pluginHandle = kPluginHandle_Invalid;

OBSEMessagingInterface* g_msg;

void UnifiedMessageHandler(OBSEMessagingInterface::Message* msg)
{
	KeywordAPI::MessageHandler(msg);
}

void MessageHandler(OBSEMessagingInterface::Message* msg)
{
	switch (msg->type)
	{
	case OBSEMessagingInterface::kMessage_PostLoad:
		g_msg->RegisterListener(g_pluginHandle, nullptr, UnifiedMessageHandler);
		break;
	case OBSEMessagingInterface::kMessage_LoadGame:
		MediumArmor::InstallHooks();
		break;
	default:
		break;
	}
}

extern "C" {

	bool OBSEPlugin_Query(const OBSEInterface* obse, PluginInfo* info)
	{

		// fill out the info structure
		info->infoVersion = PluginInfo::kInfoVersion;
		info->name = "MediumArmor";
		info->version = 1;

		// version checks
		if (!obse->isEditor)
		{
			if (obse->obseVersion < OBSE_VERSION_INTEGER)
			{
				_ERROR("OBSE version too old (got %u expected at least %u)", obse->obseVersion, OBSE_VERSION_INTEGER);
				return false;
			}

#if OBLIVION
			if (obse->oblivionVersion != OBLIVION_VERSION)
			{
				_ERROR("incorrect Oblivion version (got %08X need %08X)", obse->oblivionVersion, OBLIVION_VERSION);
				return false;
			}
#endif
		}

		return true;
	}

	bool OBSEPlugin_Load(OBSEInterface* OBSE)
	{
		g_pluginHandle = OBSE->GetPluginHandle();

		g_msg = static_cast<OBSEMessagingInterface*>(OBSE->QueryInterface(kInterface_Messaging));
		g_msg->RegisterListener(g_pluginHandle, "OBSE", MessageHandler);

		KeywordAPI::Init(g_msg, g_pluginHandle);

		return true;
	}

};