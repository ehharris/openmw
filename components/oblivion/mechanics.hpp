#ifndef OPENMW_COMPONENTS_OBLIVION_MECHANICS_H
#define OPENMW_COMPONENTS_OBLIVION_MECHANICS_H

/*
  Copyright (C) 2026 The OpenMW Developers

  This software is provided 'as-is', without any express or implied
  warranty.  In no event will the authors be held liable for any damages
  arising from the use of this software.

  Permission is granted to anyone to use this software for any purpose,
  including commercial applications, and to alter it and redistribute it
  freely, subject to the following restrictions:

  1. The origin of this software must not be misrepresented; you must not
     claim that you wrote the original software. If you use this software
     in a product, an acknowledgment in the product documentation would be
     appreciated but is not required.
  2. Altered source versions must be plainly marked as such, and must not be
     misrepresented as being the original software.
  3. This notice may not be removed or altered from any source distribution.
*/

/*
  Oblivion-specific gameplay mechanics.

  This component implements the rule systems of The Elder Scrolls IV:
  Oblivion that differ from the Morrowind rules implemented by the engine
  (apps/openmw/mwmechanics). It is deliberately self-contained: nothing
  here depends on the UI, the world or the esm3 stores, so the rules can be
  unit tested and driven by the engine's Oblivion content pipeline
  (components/oblivion/contentloader). Numeric constants and formulas are
  taken from the game's construction set data and from the reverse
  engineered rules documented on the UESP Wiki (https://en.uesp.net/wiki/Oblivion);
  each formula cites the page it is based on.

  Responsibilities:

    - the Oblivion skill system: the 21 Oblivion skills, their governing
      attributes, specialisations, effective values (including the Luck
      modifier), skill ranks, class definitions with major skills, and the
      Oblivion level-up rules (10 major skill increases, attribute bonuses
      based on skill use, +10% of Endurance to Health per level),
    - the Oblivion magic system: the six Oblivion magic schools, the spell
      effect table (base cost, barter factor, magnitude/duration behaviour),
      spellmaking cost/requirement/gold formulas, enchantment charge
      accounting, constant-effect enchantments and soul gems,
    - the Oblivion inventory and crafting systems: item instances with
      condition/charge/stolen state, encumbrance, alchemy brewing (apparatus,
      strength formulas, effect identification), Armorer repairs and soul gem
      enchanting,
    - the Oblivion AI behaviour patterns: aggression/confidence/
      responsibility driven combat decisions, flee/yield rules, disposition
      modifiers from fame/infamy and the package categories used by PACK
      records,
    - integration with the existing game mode system: Oblivion gameplay
      modes (alchemy, enchanting, spellmaking, ...) are exposed through a
      small interface (GameModeSwitcher) that the host engine maps onto its
      own mode system (apps/openmw/mwgui/mode.hpp, MWGui::GuiMode). This
      component intentionally does not include the UI.

  None of the classes here replace the engine's generic mechanics (combat,
  movement, physics); they provide the Oblivion-flavoured rules those
  systems consult. The engine wires them up where Morrowind behaviour and
  Oblivion behaviour diverge.
*/

#include <array>
#include <cstdint>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace Oblivion
{
    /* ============ shared constants ============ */

    inline constexpr std::size_t kAttributeCount = 8;
    inline constexpr std::size_t kSkillCount = 21;
    inline constexpr std::size_t kMagicSchoolCount = 6;

    /*
      The eight Oblivion attributes. The indices match the on-disk order of
      ESM4::AttributeValues (components/esm4/actor.hpp) so Oblivion attribute
      records can be applied directly.
    */
    enum class Attribute : std::uint8_t
    {
        Strength = 0,
        Intelligence = 1,
        Willpower = 2,
        Agility = 3,
        Speed = 4,
        Endurance = 5,
        Personality = 6,
        Luck = 7
    };

    enum class Specialization : std::uint8_t
    {
        Combat = 0,
        Magic = 1,
        Stealth = 2
    };

    /*
      The 21 Oblivion skills. The indices match the book/race skill indices
      used by the ESM4 records (components/esm4/loadbook.hpp, BookSkill and
      components/esm4/loadrace.hpp, SkillIndex = index + 0x0C), which is also
      the canonical Oblivion skill ordering.
    */
    enum class Skill : std::uint8_t
    {
        Armorer = 0,
        Athletics = 1,
        Blade = 2,
        Block = 3,
        Blunt = 4,
        HandToHand = 5,
        HeavyArmor = 6,
        Alchemy = 7,
        Alteration = 8,
        Conjuration = 9,
        Destruction = 10,
        Illusion = 11,
        Mysticism = 12,
        Restoration = 13,
        Acrobatics = 14,
        LightArmor = 15,
        Marksman = 16,
        Mercantile = 17,
        Security = 18,
        Sneak = 19,
        Speechcraft = 20
    };

    std::string_view skillName(Skill skill);
    std::string_view attributeName(Attribute attribute);
    std::string_view specializationName(Specialization specialization);

    /*
      Governing attribute of a skill (https://en.uesp.net/wiki/Oblivion:Skills).
      Luck governs no skills.
    */
    Attribute governingAttribute(Skill skill);

    // Specialization of a skill; each specialization owns exactly seven skills.
    Specialization specializationOf(Skill skill);

    /*
      Acrobatics, Athletics and Speechcraft are the only skills not modified
      by Luck (https://en.uesp.net/wiki/Oblivion:Attributes#Relation_between_Attributes_and_Skills).
    */
    bool skillIgnoresLuck(Skill skill);

    inline constexpr std::int32_t kAttributeMin = 0;
    inline constexpr std::int32_t kAttributeMax = 100; // base attribute values are capped at 100
    inline constexpr std::int32_t kSkillMin = 0;
    inline constexpr std::int32_t kSkillMax = 100; // effective value may exceed 100 only via Luck, capped at 100

    // Skills are not modified by Luck below this Luck value.
    inline constexpr std::int32_t kLuckSkillThreshold = 50;
    // Each point of Luck above the threshold is worth 0.4 skill points.
    inline constexpr float kLuckSkillFactor = 0.4f;

    /*
      Effective skill value including the hidden Luck modifier
      (https://en.uesp.net/wiki/Oblivion:Attributes#Relation_between_Attributes_and_Skills):

          effective = base,                                if the skill ignores Luck
          effective = clamp(base + 0.4 * (luck - 50), 0, 100), otherwise

      The governing attribute and skill specialisation do not change the
      value; they only affect level-up bonuses and cost multipliers.
    */
    std::int32_t effectiveSkillValue(std::int32_t baseValue, Skill skill, std::int32_t luck);

    /*
      Skill mastery ranks, thresholds per the Alchemy mastery perks
      (https://en.uesp.net/wiki/Oblivion:Alchemy#Mastery):
      Novice < 25, Apprentice 25-49, Journeyman 50-74, Expert 75-99, Master 100.
    */
    enum class SkillRank : std::uint8_t
    {
        Novice = 0,
        Apprentice = 1,
        Journeyman = 2,
        Expert = 3,
        Master = 4
    };

    SkillRank skillRank(std::int32_t value);
    std::string_view skillRankName(SkillRank rank);

    /* ============ Oblivion class ============ */

    /*
      An Oblivion class: a specialization and seven major skills, plus the
      two primary attributes of the class. Major skills start at 25 (+5 more
      if the skill is also in the class specialization); every other skill
      starts at 5.
    */
    struct ClassDefinition
    {
        std::string mName;
        Specialization mSpecialization = Specialization::Combat;
        std::array<Skill, 7> mMajorSkills{};
        std::array<Attribute, 2> mPrimaryAttributes{};

        bool isMajorSkill(Skill skill) const noexcept;
        bool isInSpecialization(Skill skill) const noexcept;
    };

    /* ============ actor statistics ============ */

    struct Attributes
    {
        std::array<std::int32_t, kAttributeCount> mValues{};

        Attributes() noexcept { mValues.fill(kAttributeMin); }

        std::int32_t operator[](Attribute attribute) const noexcept;
        std::int32_t& operator[](Attribute attribute) noexcept;

        std::int32_t get(Attribute attribute) const noexcept { return (*this)[attribute]; }
        void set(Attribute attribute, std::int32_t value) noexcept;
    };

    inline constexpr std::int32_t kSkillStartingValue = 5;
    inline constexpr std::int32_t kSkillMajorStartingValue = 25;
    inline constexpr std::int32_t kSkillMajorSpecializationBonus = 5; // +5 for major skills in the class specialization

    inline constexpr std::int32_t kClassMajorSkillCount = 7;
    // A level up becomes available after 10 major skill increases
    // (https://en.uesp.net/wiki/Oblivion:Leveling).
    inline constexpr std::int32_t kMajorSkillIncreasesPerLevelUp = 10;
    // Health is increased by 10% of the Endurance attribute on level up.
    inline constexpr float kHealthGainPerEndurance = 0.1f;
    // Oblivion trainers train a skill up to five times per character level.
    inline constexpr std::int32_t kMaxTrainingsPerLevel = 5;

    struct AttributesSnapshot // for savegames; layout documented for serialization layers
    {
        std::array<std::int32_t, kAttributeCount> mValues{};
        std::array<std::int32_t, kAttributeCount> mAttributeSkillIncreases{};
        std::array<std::int32_t, kSkillCount> mBaseSkills{};
        std::int32_t mMajorSkillIncreases = 0;
        std::int32_t mTrainingsUsed = 0;
        std::int32_t mLevel = 1;
        float mHealthFromLevelUps = 0.f;
    };

    /*
      Oblivion attribute bonus on level up as a function of the number of
      skill points gained in skills governed by the attribute since the
      previous level (https://en.uesp.net/wiki/Oblivion:Leveling#Raising_Attributes):

          0       -> +1
          1-4     -> +2
          5-7     -> +3
          8-9     -> +4
          10+     -> +5 (the maximum; surplus does not roll over)
    */
    std::int32_t attributeBonusForSkillIncreases(std::int32_t increases);

    /*
      Per-actor Oblivion statistics: attributes, skills, level and the
      level-up bookkeeping (major skill increases, per-attribute skill
      increases). Used for the player character and for NPCs/creatures.
    */
    class ActorStats
    {
    public:
        ActorStats();

        // Level is 1 at creation; base health is what character creation
        // (race/class) grants. Passing 0 leaves it to the caller to set.
        ActorStats(std::int32_t initialMaxHealth, std::int32_t initialMaxMagicka, std::int32_t initialMaxFatigue);

        /* --- class --- */
        void setClass(const ClassDefinition& cls);
        const ClassDefinition* getClass() const noexcept;

        // Reset skills to Oblivion starting values: 5 for every skill,
        // 25 for the class major skills, +5 for major skills that are also
        // in the class specialization.
        void initializeFromClass();

        /* --- attributes --- */
        std::int32_t getAttribute(Attribute attribute) const noexcept;
        void setAttribute(Attribute attribute, std::int32_t value) noexcept;
        std::int32_t getEffectiveAttribute(Attribute attribute) const noexcept { return getAttribute(attribute); }

        // Derived attributes (https://en.uesp.net/wiki/Oblivion:Attributes#Derived_Attributes).
        // Magicka is +1 per point of Intelligence; Encumbrance +5 per point
        // of Strength; Fatigue +1 per point of Strength, Endurance, Agility
        // and Willpower (base 100); Health is initial + 10% of Endurance per level up.
        std::int32_t getMaxMagicka() const noexcept;
        std::int32_t getMaxFatigue() const noexcept;
        std::int32_t getMaxEncumbrance() const noexcept;
        std::int32_t getMaxHealth() const noexcept;

        /* --- skills --- */
        std::int32_t getSkill(Skill skill) const noexcept;
        void setSkill(Skill skill, std::int32_t value) noexcept;
        // Luck-adjusted value used by most gameplay formulas.
        std::int32_t getEffectiveSkill(Skill skill) const noexcept;

        /* --- experience / leveling --- */

        // Raise a skill. Both player practice and external sources (books,
        // trainers, quest rewards) funnel through here.
        // Returns true if the skill reached or passed kSkillMax on this raise
        // (the raise is still applied, clamped to kSkillMax).
        // majorSkillIncreases only counts major skills of the current class.
        bool raiseSkill(Skill skill, std::int32_t amount = 1);

        std::int32_t getMajorSkillIncreases() const noexcept;
        std::int32_t skillIncreasesForAttribute(Attribute attribute) const noexcept;
        bool isLevelUpReady() const noexcept { return mMajorSkillIncreases >= kMajorSkillIncreasesPerLevelUp; }

        // Training is limited to kMaxTrainingsPerLevel per character level.
        // Returns true and records a training if one is available.
        bool canTrain() const noexcept { return mTrainingsUsed < kMaxTrainingsPerLevel; }
        bool trainSkill(Skill skill);
        std::int32_t getTrainingsUsedThisLevel() const noexcept { return mTrainingsUsed; }

        /*
          Attribute bonus this attribute would receive on level up. Luck is
          never offered (it governs no skills).
        */
        std::int32_t attributeBonusForLevelUp(Attribute attribute) const noexcept;

        /*
          Apply a level up. chosenAttributes must be three distinct basic
          attributes (the level-up screen offers the player a choice). Each
          chosen attribute gains attributeBonusForLevelUp(); Health gains
          10% of Endurance. Major/attribute increase counters are deducted
          (10) and reset respectively; Luck is never a valid choice and is
          rejected. Returns false if level up is not available or the
          attributes are invalid.
        */
        bool levelUp(const std::array<Attribute, 3>& chosenAttributes);

        std::int32_t getLevel() const noexcept { return mLevel; }

        /* --- serialization helpers --- */
        AttributesSnapshot snapshot() const noexcept;
        void restoreFromSnapshot(const AttributesSnapshot& snapshot) noexcept;

    private:
        Attributes mAttributes;
        std::array<std::int32_t, kSkillCount> mSkills{};

        const ClassDefinition* mClass = nullptr; // non-owning; the class definition is shared/static
        ClassDefinition mOwnedClass;

        std::int32_t mLevel = 1;
        std::int32_t mInitialMaxHealth = 0;
        std::int32_t mInitialMaxMagicka = 0;
        std::int32_t mInitialMaxFatigue = 0;
        float mHealthFromLevelUps = 0.f;

        std::int32_t mMajorSkillIncreases = 0;
        std::array<std::int32_t, kAttributeCount> mAttributeSkillIncreases{};
        std::int32_t mTrainingsUsed = 0;
    };

    /* ============ Oblivion magic system ============ */

    enum class MagicSchool : std::uint8_t
    {
        Alteration = 0,
        Conjuration = 1,
        Destruction = 2,
        Illusion = 3,
        Mysticism = 4,
        Restoration = 5
    };

    Skill schoolSkill(MagicSchool school);
    std::string_view schoolName(MagicSchool school);
    // The skill that governs a school is the school index + 8 in the skill ordering.
    MagicSchool skillToSchool(Skill skill);

    // Per-school effective skills (Luck adjusted), used by cost calculations.
    using SchoolSkills = std::array<std::int32_t, kMagicSchoolCount>;

    /*
      One entry of the Oblivion spell effect table. mId is the fourCC of the
      effect (equal to ESM4::MagicEffectID values, see
      components/esm4/magiceffectid.hpp), mBaseCost is the Construction Set
      base cost (https://en.uesp.net/wiki/Oblivion:Spell_Effects), mBarterFactor
      is the constant-effect enchantment value gained per magnitude point, and
      the two booleans describe which parameter of the effect is variable.
    */
    struct SpellEffectInfo
    {
        std::uint32_t mId = 0;
        MagicSchool mSchool = MagicSchool::Alteration;
        float mBaseCost = 0.f;
        float mBarterFactor = 0.f;
        bool mDurationOnly = false; // magnitude is fixed at 1 (Invisibility, Night-Eye, Paralyze, Silence,
        // Water Breathing, Water Walking)
        bool mMagnitudeOnly = false; // duration is fixed at 1 (Dispel)
        bool mConstantEnchantable = false; // usable as a worn (constant effect) enchantment
        // Considered a negative effect for alchemy purposes (poisons vs potions and the Alembic/Retort rules).
        bool mHarmful = false;
    };

    // The full vanilla effect table. nullptr is returned for unknown ids.
    const SpellEffectInfo* findSpellEffectInfo(std::uint32_t effectId) noexcept;
    // Number of entries in the effect table.
    std::size_t numSpellEffects() noexcept;
    const SpellEffectInfo& spellEffectInfoAt(std::size_t index) noexcept;

    enum class SpellRange : std::uint8_t
    {
        Self = 0,
        Touch = 1,
        Target = 2 // Targeted spells cost 1.5x
    };

    // One effect of a spell (or of an enchantment), with the parameters
    // Oblivion spellmaking supports: magnitude, duration, area and range.
    struct SpellEffect
    {
        std::uint32_t mEffectId = 0;
        SpellRange mRange = SpellRange::Self;
        std::int32_t mMinMagnitude = 1;
        std::int32_t mMaxMagnitude = 1;
        std::int32_t mDuration = 1; // seconds; ignored for magnitude-only effects
        std::int32_t mArea = 0; // feet; 0 = no area
        std::int32_t mParam = 0; // attribute/skill index for Fortify/Damage/Drain Attribute/Skill; otherwise unused
    };

    struct Spell
    {
        std::string mName;
        std::vector<SpellEffect> mEffects;
    };

    // Casting cost of a single effect. Formula from the spellmaking rules
    // (https://en.uesp.net/wiki/Oblivion:Spell_Making#Spell_Cost):
    //
    //   B = BaseCost / 10     (clamped to at least 0)
    //   M = max(1, magnitude) ^ 1.28
    //   D = max(1, duration)
    //   A = max(1, area * 0.15)
    //   Target-range spells cost x1.5
    //   skill multiplier = 1.4 - 0.012 * effectiveSchoolSkill (skill clamped to [0, 100])
    //
    // The supplied school skill is the Luck-adjusted effective value.
    float effectMagickaCost(const SpellEffect& effect, std::int32_t effectiveSchoolSkill);

    // Cost of a single effect with no skill multiplier applied. Used for the
    // spell skill requirement and for display purposes.
    float effectBaseMagickaCost(const SpellEffect& effect);

    // Sum of the per-effect costs for the whole spell, each adjusted by the
    // effective skill of its own school (as Oblivion does for multi-effect spells).
    float spellMagickaCost(const Spell& spell, const SchoolSkills& schoolSkills);

    // Sum of the per-effect base costs (no skill adjustment). Oblivion uses
    // this for the skill requirement and the gold cost checks.
    float spellBaseMagickaCost(const Spell& spell);

    // School of the most expensive single effect (highest base cost); this
    // determines both the skill requirement school and the spell's school.
    // Returns Alteration for an empty spell.
    MagicSchool primarySpellSchool(const Spell& spell);

    struct SpellRequirement
    {
        MagicSchool mSchool = MagicSchool::Alteration;
        std::int32_t mRequiredSkill = 0; // 0 = no requirement (Novice), 25/50/75/100 otherwise
    };

    // Skill level required to cast a spell, from the total unadjusted base
    // cost of all effects (https://en.uesp.net/wiki/Oblivion:Spell_Making#Spell_Cost):
    // < 26 none, 26-62 25, 63-149 50, 150-399 75, >= 400 100.
    std::int32_t spellRequiredSkillForBaseCost(float unadjustedBaseCost);
    SpellRequirement spellSkillRequirement(const Spell& spell);

    // The gold cost of a custom spell is three times the magicka cost to the caster.
    std::int32_t spellGoldCost(const Spell& spell, const SchoolSkills& schoolSkills);

    // May the actor cast the spell right now? Checks the effective skill of
    // the primary school against the requirement and the magicka pool.
    bool canCastSpell(const Spell& spell, const SchoolSkills& schoolSkills, std::int32_t currentMagicka);

    // Build the per-school effective skills array from an actor (used by the
    // engine so spell costs always use Luck-adjusted values).
    SchoolSkills effectiveSchoolSkills(const ActorStats& stats);

    /* ============ enchantments ============ */

    enum class SoulGemType : std::uint8_t
    {
        Petty = 0,
        Lesser = 1,
        Common = 2,
        Greater = 3,
        Grand = 4,
        Black = 5 // Black soul gems hold humanoid souls but have Grand capacity
    };

    std::int32_t soulGemCapacity(SoulGemType type) noexcept; // charge points provided by a filled gem
    std::int32_t soulGemLevel(SoulGemType type) noexcept; // 1-5; Black counts as Grand (5)
    std::string_view soulGemName(SoulGemType type) noexcept;

    // maximum magicka cost per use for a custom weapon enchantment
    inline constexpr std::int32_t kMaxWeaponEnchantmentCost = 85;
    // +0.4 * (enchantment cost + remaining charge) on the item value
    inline constexpr float kEnchantedWeaponValueFactor = 0.4f;

    enum class EnchantmentType : std::uint8_t
    {
        CastWhenStrikes = 1, // weapon: fires on hit
        CastOnUse = 2, // item: fires when activated
        ConstantEffect = 4 // worn item: permanent, no charges
    };

    struct Enchantment
    {
        std::string mName;
        EnchantmentType mType = EnchantmentType::CastWhenStrikes;
        std::vector<SpellEffect> mEffects;
    };

    // Magnitude of a constant-effect enchantment on a worn item for the
    // given soul gem level (1-5). Returns 0 for effects that cannot be used
    // as worn enchantments. Data from the worn-enchantment table
    // (https://en.uesp.net/wiki/Oblivion:Enchanting#Worn_Enchantments).
    std::int32_t constantEffectEnchantmentMagnitude(std::uint32_t effectId, std::int32_t soulLevel);

    // Gold value added by a constant-effect enchantment: magnitude * barter factor.
    std::int32_t constantEffectEnchantmentValueBonus(std::uint32_t effectId, std::int32_t soulLevel);

    // Enchantment cost of one use of a weapon enchantment (uses the same
    // effect cost formula as spells, without the school skill adjustment;
    // capped at kMaxWeaponEnchantmentCost for valid custom enchantments).
    std::int32_t weaponEnchantmentChargeCost(const Enchantment& enchantment);

    // Number of times a weapon can strike/fire before it needs recharging.
    std::int32_t weaponEnchantmentUses(std::int32_t charge, std::int32_t costPerUse);

    // Value bonus of an enchanted weapon: 0.4 * (enchantment cost + remaining charge).
    float enchantedWeaponValueBonus(std::int32_t enchantmentCost, std::int32_t remainingCharge);

    /* ============ inventory & items ============ */

    enum class ItemType : std::uint8_t
    {
        Misc = 0,
        Weapon = 1,
        Armor = 2,
        Clothing = 3,
        Ingredient = 4,
        Potion = 5,
        Key = 6,
        Book = 7
    };

    std::string_view itemTypeName(ItemType type) noexcept;

    // Static definition of an item (mirrors the leveled/static record data the
    // engine loads from an Oblivion plugin).
    struct ItemDefinition
    {
        std::uint32_t mFormId = 0;
        ItemType mType = ItemType::Misc;
        std::string mName;
        float mWeight = 0.f;
        std::int32_t mValue = 0; // gold value unenchanted
        std::int32_t mMaxHealth = 0; // weapons/armor health; 0 otherwise
        bool mIsMagicItem = false;
    };

    // One stack of items, tracking the per-instance state Oblivion keeps:
    // remaining enchantment charge, item condition (health fraction; Expert
    // Armorer can repair beyond 100%, up to 125%) and the stolen flag.
    // Items whose condition or charge differ never merge; enchanted items
    // should generally be kept as separate stacks.
    struct ItemInstance
    {
        std::uint32_t mFormId = 0;
        std::int32_t mCount = 1;
        float mCondition = 1.f; // fraction of max health; 1.25f after Expert repair
        std::int32_t mCharge = 0; // remaining enchantment charge (0 for unenchanted items)
        std::int32_t mMaxCharge = 0;
        bool mStolen = false;
    };

    /*
      A container of item stacks with Oblivion encumbrance handling
      (maximum = Strength * 5, see getMaxEncumbrance).
      The definition table is not owned; it must outlive the inventory.
    */
    class Inventory
    {
    public:
        Inventory() = default;
        explicit Inventory(const std::unordered_map<std::uint32_t, ItemDefinition>& definitions);

        void setDefinitions(const std::unordered_map<std::uint32_t, ItemDefinition>& definitions) noexcept
        {
            mDefinitions = &definitions;
        }

        const ItemDefinition* getDefinition(std::uint32_t formId) const noexcept;

        std::int32_t getItemCount(std::uint32_t formId) const noexcept;
        std::size_t getStackCount() const noexcept { return mItems.size(); }

        // Add count copies of formId. Stolen copies can be enchanted, which
        // clears the flag (enchanted items are no longer treated as stolen).
        // Returns the number of items actually added.
        std::int32_t addItem(std::uint32_t formId, std::int32_t count, bool stolen = false);
        // Remove count copies. Returns true if enough copies were available.
        bool removeItem(std::uint32_t formId, std::int32_t count);
        bool hasItem(std::uint32_t formId, std::int32_t count = 1) const noexcept;

        float getTotalWeight() const noexcept;
        bool canCarry(float capacity) const noexcept;
        float getFreeCapacity(float capacity) const noexcept;

        // Total gold value of the inventory, unenchanted values only.
        std::int32_t getTotalValue() const noexcept;

        // Mark all copies of formId as not stolen (used by the enchanting
        // service, which launders stolen items).
        void clearStolenFlag(std::uint32_t formId) noexcept;

        // Set a stack's condition/charge after enchanting.
        void applyEnchantment(std::uint32_t formId) noexcept;
        void setItemCharge(std::uint32_t formId, std::int32_t charge, std::int32_t maxCharge) noexcept;

        // Repair the item via Armorer. Restores up to armorerHealthRepaired()
        // health points, capped at maxRepairCondition(effectiveArmorer) of the
        // item's maximum health. The first matching stack is repaired.
        // Returns the health points restored; 0 if the item is already at its
        // maximum repairable condition (or cannot be repaired).
        std::int32_t repairItem(std::uint32_t formId, std::int32_t effectiveArmorer);

    private:
        const std::unordered_map<std::uint32_t, ItemDefinition>* mDefinitions = nullptr; // non-owning
        std::vector<ItemInstance> mItems;
    };

    /* ============ crafting: Armorer repair ============ */

    // Health points repaired per hammer use: Armorer + 3
    // (https://en.uesp.net/wiki/Oblivion:Armorer#Skill_Benefits).
    inline constexpr std::int32_t kArmorerRepairBase = 3;
    std::int32_t armorerHealthRepaired(std::int32_t effectiveArmorer) noexcept;

    // Items can be repaired to 100% condition; an Expert Armorer (75+) can
    // repair them up to 125%.
    float maxRepairCondition(std::int32_t effectiveArmorer) noexcept;

    // Journeyman Armorer (50+) can repair magic items; Novice/Apprentice cannot.
    bool canRepairMagicItems(std::int32_t effectiveArmorer) noexcept;

    /*
      Whether a repair hammer breaks on use. Master Armorer (100) never
      breaks hammers; an Apprentice Armorer (25-49) doubles the lifetime of
      hammers, halving the break chance. roll is a caller-supplied value in
      [0, 1) so the behaviour is testable and deterministic.
    */
    bool repairHammerBreaks(std::int32_t effectiveArmorer, float roll,
        std::int32_t baseBreakChancePercent = 20) noexcept;

    /* ============ crafting: alchemy ============ */

    enum class ApparatusQuality : std::uint8_t
    {
        Novice = 0,
        Apprentice = 1,
        Journeyman = 2,
        Expert = 3,
        Master = 4
    };

    // Apparatus strengths, used in the potion strength formulas.
    // (https://en.uesp.net/wiki/Oblivion:Alchemy#Strength_of_Apparatus)
    float apparatusStrength(ApparatusQuality quality) noexcept;

    // Effective Alchemy for potion strength and price: base Alchemy + 0.4 *
    // (Luck - 50), clamped to [0, 100]. Fortify/Damage Alchemy effects do
    // NOT apply (hardcoded base values); only the Mortar & Pestle strength
    // is added separately.
    std::int32_t effectiveAlchemyValue(std::int32_t baseAlchemy, std::int32_t baseLuck) noexcept;

    // Number of ingredient effects identified at the given Alchemy level:
    // Novice 1, Apprentice 2, Journeyman 3, Expert 4 (https://en.uesp.net/wiki/Oblivion:Alchemy#Skill_Benefits).
    std::int32_t alchemyIdentifiedEffects(std::int32_t alchemyValue) noexcept;

    // A Master Alchemist (Alchemy 100) can brew a potion from a single
    // ingredient; only the first effect of that ingredient is transferred.
    bool alchemyCanBrewSingleIngredient(std::int32_t alchemyValue) noexcept;

    /*
      The apparatus available for a brew. A Mortar and Pestle is required;
      Retort/Calcinator/Alembic are optional. strength is read through
      apparatusStrength().
    */
    struct AlchemyApparatus
    {
        ApparatusQuality mMortar = ApparatusQuality::Novice;
        std::uint8_t mHasRetort : 1 = 0;
        std::uint8_t mHasCalcinator : 1 = 0;
        std::uint8_t mHasAlembic : 1 = 0;
        ApparatusQuality mRetort = ApparatusQuality::Novice;
        ApparatusQuality mCalcinator = ApparatusQuality::Novice;
        ApparatusQuality mAlembic = ApparatusQuality::Novice;
    };

    struct AlchemyIngredient
    {
        // Effects (fourCC spell effect ids) the alchemist has identified on
        // this ingredient, in the order the record lists them.
        std::vector<std::uint32_t> mKnownEffects;
        float mWeight = 0.1f;
    };

    struct PotionEffect
    {
        std::uint32_t mEffectId = 0;
        std::int32_t mMagnitude = 1;
        std::int32_t mDuration = 1;
    };

    /*
      Brew a potion (or poison) from the given ingredients.

      Rules (https://en.uesp.net/wiki/Oblivion:Alchemy#Calculating_Potion_Strengths):
        - a Mortar and Pestle is required (strength added to effective Alchemy),
        - only identified effects are used; an effect must appear on at least
          two of the ingredients (or one, if alchemyCanBrewSingleIngredient()),
        - positive effects are boosted by a Retort (+50% magnitude, +100% duration)
          and a Calcinator (+35% magnitude/duration, or +140% if both are present),
        - negative effects are reduced by an Alembic in potions (never below 1);
          poisons are unaffected by an Alembic,
        - final magnitude/duration are rounded half up, minimum 1.

      Returns the effects of the brew; an empty vector means no common effect
      was found and nothing can be brewed.
    */
    std::vector<PotionEffect> brewPotion(const std::vector<AlchemyIngredient>& ingredients,
        std::int32_t baseAlchemy, std::int32_t baseLuck, const AlchemyApparatus& apparatus);

    // True if the brewed effects contain no positive effects (i.e. it is a poison).
    bool isPoison(const std::vector<PotionEffect>& effects) noexcept;

    // Weight of a brewed potion: the average weight of the ingredients used.
    float potionWeight(const std::vector<AlchemyIngredient>& ingredients) noexcept;

    // Sale price of a brewed potion: floor((Effective_Alchemy + Mortar * 25) * 0.45).
    std::int32_t potionSalePrice(std::int32_t baseAlchemy, std::int32_t baseLuck, ApparatusQuality mortar) noexcept;

    /* ============ Oblivion AI behaviour ============ */

    /*
      The NPC behaviour attributes (https://en.uesp.net/wiki/Oblivion:Attributes#NPC_Attributes).
      Mirrors the AIData struct in NPC_/CREA records (components/esm4/actor.hpp).
      Disposition is included here for convenience (it is per actor-pair, but
      the Oblivion rules consult it constantly).
    */
    struct AiData
    {
        // 0: never attacks; <= 5: never initiates but fights in self-defense; >= 106: attacks anyone
        std::int32_t mAggression = 5;
        std::int32_t mConfidence = 50; // percentage of health the actor is willing to lose before fleeing
        std::int32_t mEnergyLevel = 50; // frequency/range of wander package moves
        std::int32_t mResponsibility = 50; // crime morality: > 30 means the actor will report crimes
        std::int32_t mDisposition = 50; // toward the actor of interest (e.g. the player)
        std::int32_t mMood = 50; // 0-100; plays into dialogue
    };

    // How an NPC feels about the player, coarsely.
    enum class DispositionTone : std::uint8_t
    {
        Hostile = 0, // <= 20
        Neutral = 1, // 21-69
        Friendly = 2 // >= 70
    };

    DispositionTone dispositionTone(std::int32_t disposition) noexcept;

    // Attack when the target's disposition drops more than 5 points below
    // the aggression level; at aggression >= 106 the actor attacks everyone
    // (https://en.uesp.net/wiki/Oblivion:Aggression).
    inline constexpr std::int32_t kCombatDispositionMargin = 5;
    inline constexpr std::int32_t kBerserkAggression = 106;
    bool shouldInitiateCombat(std::int32_t aggression, std::int32_t disposition) noexcept;

    /*
      Flee once the actor has lost at least (100 - confidence)% of max health
      (https://en.uesp.net/wiki/Oblivion:Confidence). A 0-confidence actor
      flees immediately; a 100-confidence actor never flees.
    */
    bool shouldFleeCombat(std::int32_t confidence, float healthFraction) noexcept;

    /*
      An actor yields (offers to surrender) only when almost dead and not
      enraged. This models the vanilla "low health -> yield" behaviour; the
      threshold is deliberately narrow (<= 10% health) because vanilla
      surrender is rare and only sensible in a one-on-one fight. An actor
      whose disposition towards its aggressor is deeply negative (a zealot or
      a mortal enemy) will not yield.
    */
    inline constexpr float kYieldHealthFraction = 0.10f;
    bool shouldYield(std::int32_t confidence, float healthFraction, std::int32_t disposition) noexcept;

    // NPCs with responsibility above this will report witnessed crimes.
    inline constexpr std::int32_t kCrimeReportResponsibility = 30;
    bool willReportCrime(std::int32_t responsibility) noexcept;

    /*
      Disposition modifiers from the player's fame/infamy
      (https://en.uesp.net/wiki/Oblivion:Disposition):
        Fame:   +3 per 10 fame points, capped at +20 (reached at 67 fame).
        Infamy: -1 per 2 infamy at responsibility 100, -1 per 16 at
                responsibility 25, scaled linearly in between; at very low
                responsibility infamy instead *increases* disposition
                (up to +20). Both capped at +/-20.
    */
    std::int32_t dispositionModifierFromFame(std::int32_t fame) noexcept;
    std::int32_t dispositionModifierFromInfamy(std::int32_t infamy, std::int32_t responsibility) noexcept;

    // How the actor behaves while awake but not fighting.
    enum class BehaviorState : std::uint8_t
    {
        Wander = 0,
        Follow = 1,
        Stay = 2,
        Travel = 3,
        Patrol = 4,
        Eat = 5,
        Sleep = 6,
        Activate = 7,
        UseItem = 8,
        Fight = 9,
        Flee = 10,
        Yield = 11
    };

    // Logical package categories mirroring the PACK record types
    // (PKDT::type in components/esm4/loadpack.hpp). Numeric values are not
    // the on-disk record type ids; the engine maps them.
    enum class PackageType : std::uint8_t
    {
        Wander = 0,
        Follow = 1,
        Stay = 2,
        Travel = 3,
        Eat = 4,
        Sleep = 5,
        Patrol = 6,
        Activate = 7,
        UseItem = 8
    };

    struct AiContext
    {
        AiData mData;
        float mHealthFraction = 1.f; // current / max health
        float mFatigueFraction = 1.f;
        bool mTargetVisible = false; // an actor the AI might attack is in sight
        bool mTargetIsFoe = false; // the target is a declared enemy
        bool mCrimeWitnessed = false; // did the AI witness a crime it cares about
        PackageType mCurrentPackage = PackageType::Wander;
    };

    struct AiDecision
    {
        BehaviorState mState = BehaviorState::Wander;
        bool mShouldInitiateCombat = false; // calculation result, for callers that want the raw check
        bool mReportsCrime = false;
    };

    // Decide what an Oblivion NPC does this tick. Combat overrides the
    // package; otherwise the package drives behaviour. This is the entry
    // point the engine's AI packages call.
    AiDecision evaluateAiBehavior(const AiContext& context) noexcept;

    // Backwards-compatible free-function view of attack/flee/yield decisions.
    BehaviorState chooseCombatBehavior(const AiContext& context) noexcept;

    /* ============ game modes ============ */

    /*
      Oblivion gameplay modes relevant to the mechanics system. The host
      engine maps these onto its own UI mode system (apps/openmw: MWGui::GuiMode,
      see apps/openmw/mwgui/mode.hpp - e.g. SpellMaking -> GM_SpellCreation,
      Enchanting -> GM_Enchanting, Alchemy -> GM_Alchemy, Repair -> GM_Repair,
      Training -> GM_Training, LevelUp -> GM_Levelup, Barter -> GM_Barter,
      SpellBuying -> GM_SpellBuying, Container -> GM_Container,
      Companion -> GM_Companion, Rest -> GM_Rest, Recharge -> GM_Recharge,
      MerchantRepair -> GM_MerchantRepair). This component does not depend on the UI.
    */
    enum class GameMode : std::uint8_t
    {
        Game = 0, // GM_None: normal play
        Inventory = 1, // GM_Inventory
        Container = 2, // GM_Container
        Companion = 3, // GM_Companion
        Barter = 4, // GM_Barter
        Repair = 5, // GM_Repair
        SpellBuying = 6, // GM_SpellBuying
        SpellMaking = 7, // GM_SpellCreation
        Enchanting = 8, // GM_Enchanting
        Recharge = 9, // GM_Recharge
        Training = 10, // GM_Training
        MerchantRepair = 11, // GM_MerchantRepair
        Alchemy = 12, // GM_Alchemy
        LevelUp = 13, // GM_Levelup
        Rest = 14 // GM_Rest
    };

    std::string_view gameModeName(GameMode mode) noexcept;

    /*
      Interface implemented by the host engine to switch UI modes. The
      mechanics system notifies the switcher of requested mode changes; the
      engine is free to reject them (requestGameMode returns false).
    */
    class GameModeSwitcher
    {
    public:
        virtual ~GameModeSwitcher() = default;
        virtual bool switchToMode(GameMode mode) = 0;
    };

    /*
      Entry point for the Oblivion mechanics systems, integrating with the
      existing game mode system. Owns no engine objects; the switcher stays
      alive for as long as it is attached.
    */
    class MechanicsSystem
    {
    public:
        MechanicsSystem() = default;
        explicit MechanicsSystem(GameModeSwitcher* switcher) noexcept
            : mSwitcher(switcher)
        {
        }

        // Non-owning; may be null (mechanics then run in headless/test mode).
        void attachModeSwitcher(GameModeSwitcher* switcher) noexcept { mSwitcher = switcher; }
        GameModeSwitcher* getModeSwitcher() const noexcept { return mSwitcher; }

        GameMode getGameMode() const noexcept { return mGameMode; }
        bool isInGameMode() const noexcept { return mGameMode != GameMode::Game; }

        // Request a mode change. If a switcher is attached, it decides whether
        // the change happens; the requesting caller is notified via the return value.
        bool requestGameMode(GameMode mode) noexcept;
        // Return to Game mode, bypassing the switcher rejection (used when the
        // player confirms closing the window).
        void leaveGameMode() noexcept;

    private:
        GameModeSwitcher* mSwitcher = nullptr; // non-owning
        GameMode mGameMode = GameMode::Game;
    };
}
#endif /* OPENMW_COMPONENTS_OBLIVION_MECHANICS_H */
