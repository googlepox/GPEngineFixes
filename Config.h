#pragma once
// ============================================================================
//  MediumArmor OBSE Plugin – Config.h
//  Shared constants, version info, and tuning knobs.
// ============================================================================

namespace MediumArmor
{

	// ── Plugin metadata ────────────────────────────────────────────────────────
	constexpr const char* kPluginName = "MediumArmor";
	constexpr unsigned     kPluginVersion = 1;  // bump on release

	// ── OBSE minimum version required ──────────────────────────────────────────
	constexpr unsigned kOBSEVersionRequired = 21;  // OBSE v0021+

	// ── Keyword that your Keyword mod attaches to medium-class armors ──────────
	//    Change this to match whatever tag string your keyword plugin uses.
	constexpr const char* kMediumArmorKeyword = "MediumArmor";

	// ── MenuQue messaging ──────────────────────────────────────────────────────
	//    The plugin name MenuQue registers with OBSEMessagingInterface.
	constexpr const char* kMenuQueName = "MenuQue";

	// ── AR formula tuning ──────────────────────────────────────────────────────
	//    Medium AR = baseAR * (skill / 100) * condition * kARMultiplier + kARFlat
	//    Tweak these to taste; they let medium armor sit between light and heavy.
	constexpr float kARMultiplier = 1.0f;   // multiplicative scaling factor
	constexpr float kARFlat = 0.0f;   // additive bonus/penalty

	// ── Mastery perks ─────────────────────────────────────────────────────────
	//    Mastery thresholds (same as vanilla):
	//      0 = Novice (0–24)   1 = Apprentice (25–49)
	//      2 = Journeyman (50–74)   3 = Expert (75–99)   4 = Master (100)
	//
	//    Degradation multipliers (applied to Calc_DamageToArmor result):
	constexpr float kPerkNoviceDamageMult = 1.5f;   // 150% degradation
	constexpr float kPerkApprenticeDamageMult = 1.0f;   // normal
	constexpr float kPerkJourneymanDamageMult = 0.5f;   // 50% slower

	//    Encumbrance multipliers (applied to per-piece weight for equipped medium):
	constexpr float kPerkExpertEncumbranceMult = 0.5f;   // 50% weight
	constexpr float kPerkMasterEncumbranceMult = 0.0f;   // weightless

	//    Master AR bonus (applied after Calc_ArmorRating, full set only):
	constexpr float kPerkMasterARBonus = 1.25f;  // +25% AR
	constexpr int   kFullMediumSetCount = 5;      // helmet+cuirass+greaves+boots+gauntlets

	// ── Skill XP tuning ───────────────────────────────────────────────────────
	//    XP granted per hit while wearing at least one piece of medium armor.
	constexpr float kXPPerHit = 1.0f;
	constexpr float kXPSkillFactor = 0.5f;  // higher skill = slower gains

	// ── Co-save chunk type (for any extra serialization beyond MenuQue) ───────
	constexpr unsigned kCoSaveChunkID = 'MARM';

}