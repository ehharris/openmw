#include "tes4defaults.hpp"

#include <cctype>
#include <string_view>

#include <components/esm/attr.hpp>
#include <components/esm/refid.hpp>
#include <components/esm3/loadgmst.hpp>
#include <components/esm3/loadnpc.hpp>
#include <components/esm3/loadrace.hpp>
#include <components/esm3/loadskil.hpp>
#include <components/esm3/variant.hpp>

#include "esmstore.hpp"

namespace MWWorld
{
    namespace
    {
        struct DefaultGameSetting
        {
            std::string_view mId;
            float mValue;
        };

        /// Mandatory ESM3 GMST records that engine code dereferences unconditionally. TES4 content files do not
        /// provide these, so a TES4-only session would crash at startup without them. Grow this table as new
        /// startup failures are found.
        constexpr DefaultGameSetting sDefaultGameSettings[] = {
            { "fSwimHeightScale", 1.35f },
            // Unarmored armour rating formula (MWWorld::Npc::getModifiedRatingFromHealth via store.find).
            { "fUnarmoredBase1", 6.f },
            { "fUnarmoredBase2", 0.03f },
        };

        struct DefaultSkill
        {
            int mIndex; // ESM::Skill::indexToRefId
            int mAttribute; // ESM::Attribute::indexToRefId
            int mSpecialization; // 0 combat, 1 magic, 2 stealth (vanilla values)
        };

        /// The Morrowind-dialect skill vocabulary. Engine code (NpcStats, Lua mechanics scripts) dereferences these
        /// skill ids unconditionally; TES4 content provides its own SKIL records that are not bridged into the esm3
        /// Skill store. Grow/adjust this table if engine code references another skill id at startup.
        constexpr DefaultSkill sDefaultSkills[] = {
            { 0, 0, 0 },  // block - Strength
            { 1, 0, 0 },  // armorer - Strength
            { 2, 0, 0 },  // mediumArmor - Strength
            { 3, 0, 0 },  // heavyArmor - Strength
            { 4, 0, 0 },  // bluntWeapon - Strength
            { 5, 0, 0 },  // longBlade - Strength
            { 6, 0, 0 },  // axe - Strength
            { 7, 0, 0 },  // spear - Strength
            { 8, 3, 2 },  // athletics - Agility
            { 9, 1, 1 },  // enchant - Intelligence
            { 10, 1, 1 }, // destruction - Intelligence
            { 11, 1, 1 }, // alteration - Intelligence
            { 12, 1, 1 }, // illusion - Intelligence
            { 13, 1, 1 }, // conjuration - Intelligence
            { 14, 1, 1 }, // mysticism - Intelligence
            { 15, 1, 1 }, // restoration - Intelligence
            { 16, 1, 1 }, // alchemy - Intelligence
            { 17, 2, 0 }, // unarmored - Willpower
            { 18, 3, 2 }, // security - Agility
            { 19, 3, 2 }, // sneak - Agility
            { 20, 3, 2 }, // acrobatics - Agility
            { 21, 4, 0 }, // lightArmor - Speed
            { 22, 3, 0 }, // shortBlade - Agility
            { 23, 3, 0 }, // marksman - Agility
            { 24, 7, 2 }, // mercantile - Luck
            { 25, 7, 2 }, // speechcraft - Luck
            { 26, 3, 0 }, // handToHand - Agility
        };
    }

    void seedTES4DefaultGameSettings(ESMStore& store)
    {
        Store<ESM::GameSetting>& gmsts = store.getWritable<ESM::GameSetting>();

        for (const auto& defaultSetting : sDefaultGameSettings)
        {
            const ESM::RefId id = ESM::RefId::stringRefId(defaultSetting.mId);
            if (gmsts.search(id) != nullptr)
                continue;

            ESM::GameSetting setting;
            setting.blank();
            setting.mId = id;
            setting.mValue = ESM::Variant(defaultSetting.mValue);
            store.insertStatic(setting);
        }
    }

    void seedTES4DefaultSkills(ESMStore& store)
    {
        Store<ESM::Skill>& skills = store.getWritable<ESM::Skill>();

        for (const auto& defaultSkill : sDefaultSkills)
        {
            const ESM::RefId id = ESM::Skill::indexToRefId(defaultSkill.mIndex);
            if (skills.search(id) != nullptr)
                continue;

            ESM::Skill skill;
            skill.blank();
            skill.mId = id;
            std::string name = id.toString();
            if (!name.empty())
                name[0] = static_cast<char>(std::toupper(static_cast<unsigned char>(name[0])));
            skill.mName = std::move(name);
            skill.mData.mAttribute = ESM::Attribute::indexToRefId(defaultSkill.mAttribute);
            skill.mData.mSpecialization = defaultSkill.mSpecialization;
            store.insertStatic(skill);
        }
    }

    void seedTES4DefaultPlayer(ESMStore& store)
    {        Store<ESM::Race>& races = store.getWritable<ESM::Race>();
        const ESM::RefId raceId = ESM::RefId::stringRefId("playerrace");
        if (races.search(raceId) == nullptr)
        {
            // Minimal esm3-dialect race backing the synthetic Player. Body-part and skin lookups against an empty
            // part list degrade to invisible meshes, which is acceptable for startup; missing Race records are not.
            ESM::Race race;
            race.blank();
            race.mId = raceId;
            race.mName = "Player";
            race.mData.mMaleHeight = race.mData.mFemaleHeight = 1.0f;
            race.mData.mMaleWeight = race.mData.mFemaleWeight = 1.0f;
            race.mData.mFlags = ESM::Race::Playable;
            store.insertStatic(race);
        }

        Store<ESM::NPC>& npcs = store.getWritable<ESM::NPC>();

        const ESM::RefId id = ESM::RefId::stringRefId("Player");
        if (npcs.search(id) != nullptr)
            return;

        // Minimal esm3-dialect player base object. The Morrowind runtime dereferences the Player NPC during world
        // initialisation; TES4 content provides no equivalent usable record (vanilla Oblivion's is flagged invalid).
        // Attributes and skills are left empty: NPDTstruct52 lookups default to 0, which is sufficient for startup.
        ESM::NPC npc;
        npc.blank();
        npc.mId = id;
        npc.mName = "Player";
        npc.mRace = raceId;
        npc.mNpdtType = ESM::NPC::NPC_DEFAULT;
        npc.mNpdt.mLevel = 1;
        npc.mNpdt.mHealth = 50;
        npc.mNpdt.mMana = 25;
        npc.mNpdt.mFatigue = 100;
        store.insertStatic(npc);
    }
}
