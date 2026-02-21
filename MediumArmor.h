#pragma once

#include "obse/GameForms.h"
#include "obse/GameObjects.h"

namespace MediumArmor
{

	bool IsMediumArmor(TESForm* form);

	bool HasKeyword(TESForm* form, const char* keyword);

	float GetMediumArmorSkill();

	void  SetMediumArmorSkill(float value);

	void  SyncSkillFromMenuQue();

	float CalculateXPGain(float currentSkill);

	void  AwardXP(float xp);


	int   CountEquippedMediumArmor(Actor* actor);

	bool  IsWearingMediumArmor(Actor* actor);

}