#pragma once
// ============================================================================
//  MediumArmor OBSE Plugin – MediumArmor.h
//  Core gameplay logic: keyword-based classification, skill value tracking,
//  and XP gain helpers.  The actual skill *variable* lives in the MenuQue
//  custom-skill system (exposed via your .esp); this module mirrors / caches
//  it on the native side so the AR hook can read it at zero script overhead.
// ============================================================================

#include "obse/GameForms.h"
#include "obse/GameObjects.h"

namespace MediumArmor
{

	// ── Keyword interface ──────────────────────────────────────────────────────
	//    These are thin wrappers you should adapt to match whatever API your
	//    Keyword mod exposes.  Two common patterns:
	//
	//    (A) Your keyword plugin exposes an OBSE messaging callback that answers
	//        "does form X have keyword Y?"  → implement via the messaging interface.
	//    (B) Your keyword plugin stores data in extra-data or a lookup table
	//        that you can query directly from C++.
	//
	//    The default implementation below is a stub that always returns false.
	//    Replace the body of HasKeyword() in MediumArmor.cpp.

	/// Returns true if `form` carries the medium-armor keyword.
	bool IsMediumArmor(TESForm* form);

	/// Lower-level: returns true if `form` has the given keyword string.
	/// Wire this up to your Keyword mod's API.
	bool HasKeyword(TESForm* form, const char* keyword);

	// ── Skill cache ────────────────────────────────────────────────────────────
	//    The authoritative skill value is owned by MenuQue / your .esp.
	//    Call SyncSkillFromMenuQue() once per frame (or on message) to pull the
	//    latest value so native hooks can use it without calling back into script.

	/// Get the cached medium-armor skill value (0–100).
	float GetMediumArmorSkill();

	/// Set the cached skill value (called from SyncSkillFromMenuQue or command).
	void  SetMediumArmorSkill(float value);

	/// Pull the current value from MenuQue's custom-skill system.
	/// Call this from your frame or message handler.
	void  SyncSkillFromMenuQue();

	// ── XP helpers ─────────────────────────────────────────────────────────────

	/// Calculate XP to grant for a single armour hit at the current skill level.
	float CalculateXPGain(float currentSkill);

	/// Award XP through the MenuQue custom skill (calls back into script/API).
	void  AwardXP(float xp);

	// ── Equipped-set helpers ───────────────────────────────────────────────────

	/// Count how many equipped armour slots on `actor` are medium armour.
	int   CountEquippedMediumArmor(Actor* actor);

	/// Returns true if the actor has at least one medium-armour piece equipped.
	bool  IsWearingMediumArmor(Actor* actor);

	/// Returns true if actor is wearing a full set (5 pieces) of medium armour.
	bool  IsWearingFullMediumSet(Actor* actor);

	/// Returns mastery level (0=Novice .. 4=Master) based on current medium skill.
	int   GetMediumArmorMasteryLevel();

}  // namespace MediumArmorPlugin