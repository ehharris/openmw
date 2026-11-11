// Oblivion (TES IV) support: integration tests.
//
// These tests exercise the Oblivion-specific components together with the rest of the
// engine's infrastructure, as a complete session would:
//
//   - the [Oblivion] section of files/settings-default.cfg and the Settings system
//     (components/settings),
//   - the Oblivion command line options registered on the engine's options description
//     (components/oblivion/config.hpp, wired into apps/openmw/options.cpp),
//   - the startup content check that runs over the configured content files
//     (components/oblivion/config.hpp, called from apps/openmw/main.cpp),
//   - the Oblivion content loading system
//     (components/oblivion/contentloader.hpp, used by apps/openmw/mwworld/esmloader.cpp),
//   - the generic TES4-family streaming reader the engine hands records to
//     (components/esm4/reader.hpp),
//   - the Oblivion gameplay rules (components/oblivion/mechanics.hpp): leveling, luck
//     modified skills, spellmaking, enchantment, alchemy, inventory, AI behaviour and
//     the game mode switcher that ties the mechanics system into the engine's UI modes.
//
// The tests build synthetic Oblivion plugin files in a temporary directory, so they do
// not require any game data. They live at the path requested by the task brief; they are
// built as part of the components-tests target (see apps/components_tests/CMakeLists.txt),
// which is where all component and integration tests in this repository are built and run.

#include <components/oblivion/contentloader.hpp>
#include <components/oblivion/mechanics.hpp>
#include <components/oblivion/config.hpp>

#include <components/esm/common.hpp>
#include <components/esm/format.hpp>
#include <components/esm/fourcc.hpp>
#include <components/esm/gameformat.hpp>
#include <components/esm4/magiceffectid.hpp>
#include <components/esm4/reader.hpp>
#include <components/files/collections.hpp>
#include <components/files/openfile.hpp>
#include <components/files/oblivion/esm.hpp>
#include <components/settings/values.hpp>
#include <components/testing/util.hpp>

#include <boost/program_options/options_description.hpp>
#include <boost/program_options/parsers.hpp>
#include <boost/program_options/variables_map.hpp>

#include <gtest/gtest.h>

#include <array>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <sstream>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

#include <cmath>

#ifndef OPENMW_PROJECT_SOURCE_DIR
#define OPENMW_PROJECT_SOURCE_DIR "."
#endif

namespace
{
    namespace bpo = boost::program_options;

    // A mock of the host engine's mode system: the mechanics system drives it to switch
    // between Oblivion gameplay modes (alchemy, spellmaking, ...) and asks it for permission.
    class RecordingGameModeSwitcher : public Oblivion::GameModeSwitcher
    {
    public:
        bool switchToMode(Oblivion::GameMode mode) override
        {
            if (mReject)
                return false;
            mRequestedModes.push_back(mode);
            return true;
        }

        bool mReject = false;
        std::vector<Oblivion::GameMode> mRequestedModes;
    };

    void expectNoErrors(const Oblivion::ContentReport& report)
    {
        if (report.hasErrors())
        {
            std::ostringstream stream;
            for (const Oblivion::ConfigIssue& issue : report.mIssues)
                if (issue.mSeverity == Oblivion::ConfigIssueSeverity::Error)
                    stream << issue.mMessage << "\n";
            FAIL() << "unexpected Oblivion content check errors:\n" << stream.str();
        }
    }

    // ---- minimal synthetic plugin builder (subset of what test_oblivion_loading.cpp uses) ----

    struct BytesBuilder
    {
        std::vector<char> bytes;

        void u8(std::uint8_t value) { bytes.push_back(static_cast<char>(value)); }
        void u16(std::uint16_t value)
        {
            u8(static_cast<std::uint8_t>(value & 0xff));
            u8(static_cast<std::uint8_t>((value >> 8) & 0xff));
        }
        void u32(std::uint32_t value)
        {
            for (int shift = 0; shift < 32; shift += 8)
                u8(static_cast<std::uint8_t>((value >> shift) & 0xff));
        }
        void i32(std::int32_t value) { u32(static_cast<std::uint32_t>(value)); }
        void raw(const char* data, std::size_t size) { bytes.insert(bytes.end(), data, data + size); }
        void raw(const std::string& str) { raw(str.data(), str.size()); }
        void bytesFrom(const std::vector<char>& other) { bytes.insert(bytes.end(), other.begin(), other.end()); }
    };

    std::vector<char> makeSubRecord(std::uint32_t type, const std::vector<char>& payload)
    {
        BytesBuilder builder;
        builder.u32(type);
        if (payload.size() > 0xffff)
            throw std::logic_error("test subrecord too large");
        builder.u16(static_cast<std::uint16_t>(payload.size()));
        builder.bytesFrom(payload);
        return std::move(builder.bytes);
    }

    std::vector<char> makeStringSubRecord(std::uint32_t type, const std::string& str)
    {
        std::vector<char> payload(str.begin(), str.end());
        payload.push_back('\0');
        return makeSubRecord(type, payload);
    }

    std::vector<char> makeRecord(std::uint32_t type, std::uint32_t flags, std::uint32_t formId,
        std::uint32_t versionControlInfo, const std::vector<char>& payload)
    {
        BytesBuilder builder;
        builder.u32(type);
        builder.u32(static_cast<std::uint32_t>(payload.size()));
        builder.u32(flags);
        builder.u32(formId);
        builder.u32(versionControlInfo);
        builder.bytesFrom(payload);
        return std::move(builder.bytes);
    }

    void writeFile(const std::filesystem::path& path, const std::vector<char>& bytes)
    {
        std::ofstream stream(path, std::ios::binary);
        stream.exceptions(std::ofstream::failbit | std::ofstream::badbit);
        stream.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
    }

    struct PluginOptions
    {
        std::uint32_t hedrVersion = ESM::VER_100;
        bool master = true;
        std::int32_t numRecords = 0;
        std::string author = "Bethesda Game Studios";
        std::string description = "Test plugin";
        std::vector<std::pair<std::string, std::uint64_t>> masters;
    };

    std::vector<char> buildPlugin(const PluginOptions& options, const std::vector<char>& content = {})
    {
        BytesBuilder hedr;
        hedr.u32(options.hedrVersion);
        hedr.i32(options.numRecords);
        hedr.u32(0x00000014); // next object id
        const std::vector<char> hedrPayload = makeSubRecord(ESM::fourCC("HEDR"), hedr.bytes);

        BytesBuilder payload;
        payload.bytesFrom(hedrPayload);
        if (!options.author.empty())
            payload.bytesFrom(makeStringSubRecord(ESM::fourCC("CNAM"), options.author));
        if (!options.description.empty())
            payload.bytesFrom(makeStringSubRecord(ESM::fourCC("SNAM"), options.description));
        for (const auto& master : options.masters)
        {
            payload.bytesFrom(makeStringSubRecord(ESM::fourCC("MAST"), master.first));
            BytesBuilder data;
            data.u32(static_cast<std::uint32_t>(master.second & 0xffffffffu));
            data.u32(static_cast<std::uint32_t>(master.second >> 32));
            payload.bytesFrom(makeSubRecord(ESM::fourCC("DATA"), data.bytes));
        }

        const std::uint32_t flags = options.master ? static_cast<std::uint32_t>(Files::Oblivion::Rec_ESM) : 0u;
        std::vector<char> plugin = makeRecord(
            Files::Oblivion::REC_TES4, flags, /*formId=*/0, /*versionControlInfo=*/0, payload.bytes);
        plugin.insert(plugin.end(), content.begin(), content.end());
        return plugin;
    }

    std::filesystem::path makeDataDir(std::string_view name)
    {
        return TestingOpenMW::outputDirPath(std::filesystem::path("oblivion-integration") / std::string(name));
    }

    // Parse command-line-style arguments with the same options description the engine uses.
    bpo::variables_map parseWithOblivionOptions(const std::vector<std::string>& args)
    {
        bpo::options_description description("test options");
        Oblivion::addOptions(description);
        bpo::variables_map variables;
        bpo::store(bpo::command_line_parser(args).options(description).run(), variables);
        bpo::notify(variables);
        return variables;
    }

    // A combat class definition for character creation tests.
    Oblivion::ClassDefinition makeCombatClass()
    {
        Oblivion::ClassDefinition cls;
        cls.mName = "Test Warrior";
        cls.mSpecialization = Oblivion::Specialization::Combat;
        cls.mMajorSkills = { Oblivion::Skill::Blade, Oblivion::Skill::Block, Oblivion::Skill::Blunt,
            Oblivion::Skill::HeavyArmor, Oblivion::Skill::Armorer, Oblivion::Skill::HandToHand,
            Oblivion::Skill::Athletics };
        cls.mPrimaryAttributes = { Oblivion::Attribute::Strength, Oblivion::Attribute::Endurance };
        return cls;
    }

    Oblivion::SpellEffect makeEffect(std::uint32_t effectId, Oblivion::SpellRange range, std::int32_t minMag,
        std::int32_t maxMag, std::int32_t duration, std::int32_t area = 0)
    {
        Oblivion::SpellEffect effect;
        effect.mEffectId = effectId;
        effect.mRange = range;
        effect.mMinMagnitude = minMag;
        effect.mMaxMagnitude = maxMag;
        effect.mDuration = duration;
        effect.mArea = area;
        return effect;
    }
}

// ---------- integration with the settings system ----------

TEST(OblivionIntegration, settingsFileShipsOblivionSectionAndDefaultsMatch)
{
    // files/settings-default.cfg must ship the [Oblivion] section; components_tests/main.cpp
    // loads it into Settings::Manager before any test runs, but verify the file contents
    // directly as well so a missing section fails this test, not a distant one.
    const std::filesystem::path settingsDefaultPath = std::filesystem::path{ OPENMW_PROJECT_SOURCE_DIR } / "files"
        / "settings-default.cfg";
    std::ifstream stream(settingsDefaultPath);
    ASSERT_TRUE(stream.is_open()) << "failed to open " << settingsDefaultPath;
    const std::string contents((std::istreambuf_iterator<char>(stream)), std::istreambuf_iterator<char>());
    EXPECT_NE(contents.find("[Oblivion]"), std::string::npos);
    EXPECT_NE(contents.find("enable = true"), std::string::npos);
    EXPECT_NE(contents.find("validate content = true"), std::string::npos);
    EXPECT_NE(contents.find("strict load order = false"), std::string::npos);
    EXPECT_NE(contents.find("verbose content loading = false"), std::string::npos);

    // A fresh Values instance resolves the documented defaults.
    Settings::Index index;
    Settings::Values values(index);
    EXPECT_TRUE(values.mOblivion.mEnable.get());
    EXPECT_TRUE(values.mOblivion.mValidateContent.get());
    EXPECT_FALSE(values.mOblivion.mStrictLoadOrder.get());
    EXPECT_FALSE(values.mOblivion.mVerboseContentLoading.get());
}

// ---------- integration with the command line / options system ----------

TEST(OblivionIntegration, commandLineOptionsFeedIntoOblivionConfiguration)
{
    // --oblivion forces Oblivion mode and implies that content is validated.
    const bpo::variables_map forced = parseWithOblivionOptions({ "--oblivion" });
    const Oblivion::Configuration forcedConfig = Oblivion::loadConfiguration(forced);
    EXPECT_TRUE(forcedConfig.mForced);
    EXPECT_TRUE(forcedConfig.mEnable);
    EXPECT_TRUE(forcedConfig.mValidate); // forcing Oblivion implies validation
    EXPECT_FALSE(forcedConfig.mStrictLoadOrder); // settings.cfg default wins when the option is not given

    // Explicitly disabling validation works, too.
    const bpo::variables_map noValidate = parseWithOblivionOptions({ "--oblivion-validate=false" });
    const Oblivion::Configuration noValidateConfig = Oblivion::loadConfiguration(noValidate);
    EXPECT_FALSE(noValidateConfig.mValidate);
    EXPECT_FALSE(noValidateConfig.mForced);

    // Strict load order and verbose logging are honoured from the command line.
    const bpo::variables_map strict = parseWithOblivionOptions({ "--oblivion-strict-load-order" });
    EXPECT_TRUE(Oblivion::loadConfiguration(strict).mStrictLoadOrder);

    const bpo::variables_map verbose = parseWithOblivionOptions({ "--oblivion-verbose" });
    const Oblivion::Configuration verboseConfig = Oblivion::loadConfiguration(verbose);
    EXPECT_TRUE(verboseConfig.mVerbose);
    EXPECT_FALSE(verboseConfig.mStrictLoadOrder);

    // eq-style "--oblivion=true" syntax is supported by the same option descriptions.
    const bpo::variables_map eqStyle = parseWithOblivionOptions({ "--oblivion=true", "--oblivion-strict-load-order=true" });
    const Oblivion::Configuration eqStyleConfig = Oblivion::loadConfiguration(eqStyle);
    EXPECT_TRUE(eqStyleConfig.mForced);
    EXPECT_TRUE(eqStyleConfig.mStrictLoadOrder);
}

// ---------- integration with the startup content check ----------

TEST(OblivionIntegration, endToEndStartupCheckAndLoadingMatchesEngineBehaviour)
{
    namespace bpo = boost::program_options;

    const std::filesystem::path dir = makeDataDir("end-to-end");

    PluginOptions masterOptions;
    masterOptions.hedrVersion = ESM::VER_080; // old header version, still valid Oblivion content
    masterOptions.master = true;
    masterOptions.author = "Test Author";
    masterOptions.description = "End-to-end master";
    writeFile(dir / "EndToEndMaster.esm", buildPlugin(masterOptions));

    PluginOptions pluginOptions;
    pluginOptions.hedrVersion = ESM::VER_100;
    pluginOptions.master = false;
    pluginOptions.masters = { { "EndToEndMaster.esm", 0x00001234 } };
    writeFile(dir / "EndToEndMod.esp", buildPlugin(pluginOptions));

    const std::vector<std::string> contentFiles = { "EndToEndMaster.esm", "EndToEndMod.esp" };

    // Parse options exactly like the engine does, then run the startup content check.
    const bpo::variables_map variables = parseWithOblivionOptions({ "--oblivion" });
    const Oblivion::Configuration config = Oblivion::loadConfiguration(variables);
    ASSERT_TRUE(config.mForced);

    const Files::Collections collections({ dir });
    const Oblivion::ContentReport report = Oblivion::checkContentFiles(contentFiles, collections, config);
    EXPECT_EQ(report.mOblivionContentCount, 2u);
    expectNoErrors(report);

    // Load both files in load order, like the engine's content loading pipeline.
    const Oblivion::ContentLoader loader;
    const Oblivion::ContentFile master = loader.load(dir / "EndToEndMaster.esm");
    const Oblivion::ContentFile mod = loader.load(dir / "EndToEndMod.esp");
    EXPECT_TRUE(master.mMaster);
    EXPECT_FALSE(mod.mMaster);
    ASSERT_EQ(mod.mMasters.size(), 1u);
    EXPECT_EQ(mod.mMasters.front().mName, "EndToEndMaster.esm");
    EXPECT_TRUE(Oblivion::ContentLoader::findMissingMasters(mod, collections).empty());

    // The generic TES4-family reader the engine hands records to agrees with the loader.
    {
        const std::filesystem::path masterPath = dir / "EndToEndMaster.esm";
        ESM4::Reader esm4Master(
            Files::openBinaryInputFileStream(masterPath), masterPath, /*vfs=*/nullptr, /*encoder=*/nullptr);
        EXPECT_EQ(esm4Master.gameFormat(), ESM::GameFormat::TES4);
        EXPECT_TRUE(esm4Master.getGameFiles().empty());
        EXPECT_EQ(esm4Master.getDesc(), "End-to-end master");
    }
    {
        const std::filesystem::path modPath = dir / "EndToEndMod.esp";
        ESM4::Reader esm4Mod(
            Files::openBinaryInputFileStream(modPath), modPath, /*vfs=*/nullptr, /*encoder=*/nullptr);
        EXPECT_EQ(esm4Mod.gameFormat(), ESM::GameFormat::TES4);
        ASSERT_EQ(esm4Mod.getGameFiles().size(), 1u);
        EXPECT_EQ(esm4Mod.getGameFiles().front().name, "EndToEndMaster.esm");
    }
}

// ---------- integration with the ESM format detection ----------

TEST(OblivionIntegration, formatDetectionAndGameFormatAgreeWithLoader)
{
    const std::filesystem::path dir = makeDataDir("format-agreement");

    PluginOptions masterOptions;
    masterOptions.hedrVersion = ESM::VER_080;
    masterOptions.master = true;
    writeFile(dir / "FormatMaster.esm", buildPlugin(masterOptions));

    // ESM::readFormat recognises the on-disk format...
    auto stream = Files::openBinaryInputFileStream(dir / "FormatMaster.esm");
    ASSERT_TRUE(stream);
    EXPECT_EQ(ESM::Format::Tes4, ESM::readFormat(*stream));

    // ...the game format helpers recognise the HEDR version...
    EXPECT_EQ(ESM::getGameFormat(ESM::VER_080, std::nullopt), ESM::GameFormat::TES4);
    EXPECT_EQ(ESM::getGameFormat(ESM::VER_100, std::nullopt), ESM::GameFormat::TES4);
    EXPECT_TRUE(ESM::isOblivionVersion(ESM::VER_080));
    EXPECT_FALSE(ESM::isOblivionVersion(ESM::VER_100)); // VER_100 is shared with FO4

    // ...and the Oblivion content loader recognises both as Oblivion plugins.
    EXPECT_TRUE(Oblivion::ContentLoader::isOblivionPlugin(dir / "FormatMaster.esm"));
}

// ---------- mechanics: leveling, luck, snapshot ----------

TEST(OblivionIntegration, levelUpFlowFollowsOblivionRules)
{
    const Oblivion::ClassDefinition cls = makeCombatClass();

    Oblivion::ActorStats stats(/*initialMaxHealth=*/50, /*initialMaxMagicka=*/0, /*initialMaxFatigue=*/0);
    stats.setAttribute(Oblivion::Attribute::Strength, 40);
    stats.setAttribute(Oblivion::Attribute::Endurance, 10);
    stats.setAttribute(Oblivion::Attribute::Luck, 50);
    stats.setClass(cls);
    stats.initializeFromClass();

    // Major skills start at 25, +5 more because the class specialization is Combat.
    EXPECT_EQ(stats.getSkill(Oblivion::Skill::Blade), 30);
    EXPECT_EQ(stats.getSkill(Oblivion::Skill::Athletics), 30);
    // Minor skills start at 5.
    EXPECT_EQ(stats.getSkill(Oblivion::Skill::Alchemy), 5);

    // Raise a major skill ten times; a level up becomes available.
    for (int i = 0; i < 10; ++i)
        EXPECT_FALSE(stats.raiseSkill(Oblivion::Skill::Blade, 1));
    EXPECT_EQ(stats.getSkill(Oblivion::Skill::Blade), 40);
    EXPECT_EQ(stats.getMajorSkillIncreases(), 10);
    ASSERT_TRUE(stats.isLevelUpReady());
    EXPECT_EQ(stats.skillIncreasesForAttribute(Oblivion::Attribute::Strength), 10);
    EXPECT_EQ(stats.attributeBonusForLevelUp(Oblivion::Attribute::Strength), 5); // 10+ increases -> +5
    EXPECT_EQ(stats.attributeBonusForLevelUp(Oblivion::Attribute::Endurance), 1); // no increases -> +1
    EXPECT_EQ(stats.attributeBonusForLevelUp(Oblivion::Attribute::Luck), 0); // Luck is never offered

    const std::array<Oblivion::Attribute, 3> chosen = { Oblivion::Attribute::Strength,
        Oblivion::Attribute::Endurance, Oblivion::Attribute::Agility };
    ASSERT_TRUE(stats.levelUp(chosen));
    EXPECT_EQ(stats.getAttribute(Oblivion::Attribute::Strength), 45); // 40 + 5
    EXPECT_EQ(stats.getAttribute(Oblivion::Attribute::Endurance), 11); // 10 + 1
    EXPECT_EQ(stats.getAttribute(Oblivion::Attribute::Agility), 1); // 0 + 1
    EXPECT_EQ(stats.getLevel(), 2);
    EXPECT_EQ(stats.getMajorSkillIncreases(), 0);
    EXPECT_EQ(stats.skillIncreasesForAttribute(Oblivion::Attribute::Strength), 0);
    // Health gains 10% of the (updated) Endurance attribute per level up.
    EXPECT_EQ(stats.getMaxHealth(), 51); // 50 + floor(1.1)

    // Level up without enough major skill increases is rejected.
    EXPECT_FALSE(stats.levelUp(chosen));
}

TEST(OblivionIntegration, luckModifiesEffectiveSkillValues)
{
    const Oblivion::ClassDefinition cls = makeCombatClass();

    Oblivion::ActorStats stats(0, 0, 0);
    stats.setAttribute(Oblivion::Attribute::Luck, 50);
    stats.setClass(cls);
    stats.initializeFromClass(); // Blade is 30

    EXPECT_EQ(stats.getEffectiveSkill(Oblivion::Skill::Blade), 30); // Luck 50: no modifier

    stats.setAttribute(Oblivion::Attribute::Luck, 100);
    EXPECT_EQ(stats.getEffectiveSkill(Oblivion::Skill::Blade), 50); // +0.4 per Luck point above 50

    // Athletics is not modified by Luck.
    EXPECT_EQ(stats.getEffectiveSkill(Oblivion::Skill::Athletics), 30);
    stats.setAttribute(Oblivion::Attribute::Luck, 0);
    EXPECT_EQ(stats.getEffectiveSkill(Oblivion::Skill::Athletics), 30);
}

TEST(OblivionIntegration, snapshotRoundTripPreservesActorState)
{
    const Oblivion::ClassDefinition cls = makeCombatClass();

    Oblivion::ActorStats stats(60, 20, 80);
    stats.setAttribute(Oblivion::Attribute::Strength, 40);
    stats.setAttribute(Oblivion::Attribute::Endurance, 12);
    stats.setAttribute(Oblivion::Attribute::Luck, 70);
    stats.setClass(cls);
    stats.initializeFromClass();
    for (int i = 0; i < 10; ++i)
        stats.raiseSkill(Oblivion::Skill::Blade, 1);
    // Training a major skill also counts towards the level-up bookkeeping.
    ASSERT_TRUE(stats.trainSkill(Oblivion::Skill::Block));

    const Oblivion::AttributesSnapshot snapshot = stats.snapshot();

    // Damage the state, then restore.
    stats.setSkill(Oblivion::Skill::Blade, 5);
    stats.setAttribute(Oblivion::Attribute::Strength, 1);
    stats.restoreFromSnapshot(snapshot);

    EXPECT_EQ(stats.getSkill(Oblivion::Skill::Blade), 40);
    EXPECT_EQ(stats.getAttribute(Oblivion::Attribute::Strength), 40);
    EXPECT_EQ(stats.getMajorSkillIncreases(), 11); // 10 from practicing Blade + 1 from the training
    EXPECT_EQ(stats.getLevel(), 1);
    EXPECT_EQ(stats.getTrainingsUsedThisLevel(), 1);
    ASSERT_TRUE(stats.isLevelUpReady());
}

// ---------- mechanics: spellmaking ----------

TEST(OblivionIntegration, spellmakingUsesLuckAdjustedSchoolSkills)
{
    Oblivion::ActorStats stats(0, 20, 0);
    stats.setAttribute(Oblivion::Attribute::Intelligence, 30); // max magicka = 20 + 30
    stats.setAttribute(Oblivion::Attribute::Luck, 50); // no Luck modifier
    stats.setSkill(Oblivion::Skill::Destruction, 30);

    ASSERT_EQ(stats.getMaxMagicka(), 50);

    // A single Fire Damage strike: Touch range, magnitude 5, duration 3.
    Oblivion::Spell spell;
    spell.mName = "Flame Bolt";
    spell.mEffects = { makeEffect(ESM4::EFI_FIDG, Oblivion::SpellRange::Touch, 5, 5, 3) };

    const Oblivion::SchoolSkills skills = Oblivion::effectiveSchoolSkills(stats);
    const float baseCost = Oblivion::spellBaseMagickaCost(spell);
    const float cost = Oblivion::spellMagickaCost(spell, skills);
    ASSERT_GT(baseCost, 0.f);
    ASSERT_GT(cost, 0.f);

    // The skill requirement is derived from the unadjusted base cost: < 26 -> no requirement.
    const Oblivion::SpellRequirement requirement = Oblivion::spellSkillRequirement(spell);
    EXPECT_EQ(requirement.mSchool, Oblivion::MagicSchool::Destruction);
    EXPECT_EQ(requirement.mRequiredSkill, 0);

    EXPECT_TRUE(Oblivion::canCastSpell(spell, skills, stats.getMaxMagicka()));
    EXPECT_FALSE(Oblivion::canCastSpell(spell, skills, 0)); // not enough magicka

    // A more skilled caster pays less magicka for the same spell.
    Oblivion::ActorStats adept(0, 100, 0);
    adept.setAttribute(Oblivion::Attribute::Luck, 50);
    adept.setSkill(Oblivion::Skill::Destruction, 80);
    const Oblivion::SchoolSkills adeptSkills = Oblivion::effectiveSchoolSkills(adept);
    EXPECT_LT(Oblivion::spellMagickaCost(spell, adeptSkills), cost);

    // A weak spellcaster cannot cast a spell whose base cost demands a higher skill.
    Oblivion::Spell paralyze;
    paralyze.mName = "Paralyze";
    paralyze.mEffects = { makeEffect(ESM4::EFI_PARA, Oblivion::SpellRange::Touch, 1, 1, 99) };
    const Oblivion::SpellRequirement paralyzeRequirement = Oblivion::spellSkillRequirement(paralyze);
    EXPECT_EQ(paralyzeRequirement.mRequiredSkill, 100); // base cost >= 400
    EXPECT_EQ(paralyzeRequirement.mSchool, Oblivion::MagicSchool::Illusion);
    EXPECT_FALSE(Oblivion::canCastSpell(paralyze, skills, 10000)); // Destruction skill is irrelevant, Illusion is not

    // The gold cost is three times the magicka cost, rounded down.
    EXPECT_EQ(Oblivion::spellGoldCost(spell, skills), static_cast<std::int32_t>(std::floor(3.f * cost)));

    // An empty spell cannot be cast at all.
    const Oblivion::Spell empty;
    EXPECT_FALSE(Oblivion::canCastSpell(empty, skills, 10'000));
}

// ---------- mechanics: enchantment and soul gems ----------

TEST(OblivionIntegration, enchantmentAndSoulGemRules)
{
    EXPECT_EQ(Oblivion::soulGemCapacity(Oblivion::SoulGemType::Petty), 150);
    EXPECT_EQ(Oblivion::soulGemCapacity(Oblivion::SoulGemType::Grand), 1600);
    EXPECT_EQ(Oblivion::soulGemCapacity(Oblivion::SoulGemType::Black), 1600); // Black gems have Grand capacity
    EXPECT_EQ(Oblivion::soulGemLevel(Oblivion::SoulGemType::Black), 5);

    // Constant-effect magnitudes grow with soul level (Feather magnitudes are fixed by game data).
    EXPECT_EQ(Oblivion::constantEffectEnchantmentMagnitude(ESM4::EFI_FTHR, 1), 14);
    EXPECT_EQ(Oblivion::constantEffectEnchantmentMagnitude(ESM4::EFI_FTHR, 5), 50);
    for (std::int32_t level = 2; level <= 5; ++level)
        EXPECT_GT(Oblivion::constantEffectEnchantmentMagnitude(ESM4::EFI_FTHR, level),
            Oblivion::constantEffectEnchantmentMagnitude(ESM4::EFI_FTHR, level - 1));
    EXPECT_EQ(Oblivion::constantEffectEnchantmentValueBonus(ESM4::EFI_FTHR, 5), 1250); // 50 * 25 barter factor

    // A weapon enchantment's charge cost is the unadjusted sum of its effect base costs.
    Oblivion::Enchantment fireStrike;
    fireStrike.mName = "Fire Damage";
    fireStrike.mType = Oblivion::EnchantmentType::CastWhenStrikes;
    fireStrike.mEffects = { makeEffect(ESM4::EFI_FIDG, Oblivion::SpellRange::Touch, 3, 3, 1) };
    const std::int32_t cost = Oblivion::weaponEnchantmentChargeCost(fireStrike);
    EXPECT_EQ(cost, 3); // floor(0.75 * 3^1.28)
    EXPECT_EQ(Oblivion::weaponEnchantmentUses(16, cost), 5);
    EXPECT_EQ(Oblivion::weaponEnchantmentUses(16, 0), 0);
    EXPECT_FLOAT_EQ(Oblivion::enchantedWeaponValueBonus(cost, 16), 0.4f * 19.f);
}

// ---------- mechanics: alchemy ----------

TEST(OblivionIntegration, alchemyBrewingFollowsOblivionRules)
{
    const Oblivion::AlchemyIngredient apple{ { ESM4::EFI_REHE, ESM4::EFI_DGHE }, 0.5f };
    const Oblivion::AlchemyIngredient pear{ { ESM4::EFI_REHE, ESM4::EFI_DGHE }, 0.3f };

    Oblivion::AlchemyApparatus apparatus; // novice mortar only, no retort/calcinator/alembic
    apparatus.mMortar = Oblivion::ApparatusQuality::Novice;

    const std::vector<Oblivion::PotionEffect> potion = Oblivion::brewPotion({ apple, pear }, 50, 50, apparatus);
    ASSERT_EQ(potion.size(), 2u); // effects present on both ingredients are brewed
    for (const Oblivion::PotionEffect& effect : potion)
    {
        EXPECT_GE(effect.mMagnitude, 1);
        EXPECT_GE(effect.mDuration, 1);
    }
    EXPECT_FALSE(Oblivion::isPoison(potion)); // Restore Health is not harmful
    EXPECT_FLOAT_EQ(Oblivion::potionWeight({ apple, pear }), 0.4f);
    EXPECT_EQ(Oblivion::potionSalePrice(50, 50, Oblivion::ApparatusQuality::Novice), 23); // floor(52.5 * 0.45)

    // An all-harmful brew is a poison.
    const Oblivion::AlchemyIngredient blister{ { ESM4::EFI_DGHE }, 0.1f };
    const Oblivion::AlchemyIngredient toxic{ { ESM4::EFI_DGHE }, 0.2f };
    const std::vector<Oblivion::PotionEffect> poison = Oblivion::brewPotion({ blister, toxic }, 50, 50, apparatus);
    ASSERT_EQ(poison.size(), 1u);
    EXPECT_TRUE(Oblivion::isPoison(poison));

    // A Master Alchemist brews from a single ingredient; only the first identified effect transfers.
    const std::vector<Oblivion::PotionEffect> single = Oblivion::brewPotion({ apple }, 100, 50, apparatus);
    ASSERT_EQ(single.size(), 1u);
    EXPECT_EQ(single.front().mEffectId, ESM4::EFI_REHE);

    // A sub-master alchemist cannot brew from a single ingredient.
    EXPECT_TRUE(Oblivion::brewPotion({ apple }, 99, 50, apparatus).empty());

    EXPECT_EQ(Oblivion::alchemyIdentifiedEffects(80), 4);
    EXPECT_EQ(Oblivion::alchemyIdentifiedEffects(50), 3);
    EXPECT_EQ(Oblivion::alchemyIdentifiedEffects(25), 2);
    EXPECT_EQ(Oblivion::alchemyIdentifiedEffects(0), 1);
}

// ---------- mechanics: inventory and derived attributes ----------

TEST(OblivionIntegration, inventoryIntegratesWithActorAttributes)
{
    Oblivion::ActorStats stats(0, 0, 0);
    stats.setAttribute(Oblivion::Attribute::Strength, 20);
    stats.setAttribute(Oblivion::Attribute::Endurance, 10);
    stats.setAttribute(Oblivion::Attribute::Agility, 10);
    stats.setAttribute(Oblivion::Attribute::Willpower, 10);
    stats.setAttribute(Oblivion::Attribute::Intelligence, 25);

    // Derived attributes reflect the base attributes.
    EXPECT_EQ(stats.getMaxEncumbrance(), 100); // 5 per point of Strength
    EXPECT_EQ(stats.getMaxMagicka(), 25); // 0 (initial) + 25 (Intelligence)
    EXPECT_EQ(stats.getMaxFatigue(), 10 + 20 + 10 + 10); // 0 (initial) + 1 per Str/End/Agi/Will = 50

    const std::unordered_map<std::uint32_t, Oblivion::ItemDefinition> definitions{
        { 0x11, { 0x11, Oblivion::ItemType::Weapon, "Iron Dagger", 4.f, 25, 50, false } },
        { 0x12, { 0x12, Oblivion::ItemType::Armor, "Iron Helmet", 5.f, 100, 100, false } },
        { 0x13, { 0x13, Oblivion::ItemType::Misc, "Feather", 0.1f, 1, 0, false } },
    };

    Oblivion::Inventory inventory(definitions);
    inventory.addItem(0x11, 3, /*stolen=*/true);
    inventory.addItem(0x12, 1);
    EXPECT_EQ(inventory.getItemCount(0x11), 3);
    EXPECT_EQ(inventory.getTotalValue(), 3 * 25 + 100);
    EXPECT_FLOAT_EQ(inventory.getTotalWeight(), 17.f);
    EXPECT_TRUE(inventory.canCarry(static_cast<float>(stats.getMaxEncumbrance())));
    EXPECT_FLOAT_EQ(inventory.getFreeCapacity(100.f), 83.f);

    ASSERT_TRUE(inventory.removeItem(0x11, 2));
    EXPECT_EQ(inventory.getItemCount(0x11), 1);
    EXPECT_FALSE(inventory.removeItem(0x11, 2));
    EXPECT_TRUE(inventory.hasItem(0x11, 1));

    // The first matching stack is repaired; a pristine item needs no repair.
    EXPECT_EQ(inventory.repairItem(0x12, 50), 0);

    // Enchanting a stack clears the stolen flag as a side effect of making items non-stolen.
    inventory.addItem(0x13, 1, /*stolen=*/true);
    inventory.applyEnchantment(0x13);
    inventory.clearStolenFlag(0x11);
    EXPECT_EQ(inventory.getItemCount(0x13), 1);
}

// ---------- mechanics: AI ----------

TEST(OblivionIntegration, aiBehaviourDecidesFromNpcAttributes)
{
    Oblivion::AiContext context;
    context.mData.mAggression = 80; // aggressive
    context.mData.mConfidence = 50;
    context.mData.mResponsibility = 50;
    context.mData.mDisposition = 10; // hated
    context.mTargetVisible = true;
    context.mCurrentPackage = Oblivion::PackageType::Wander;

    EXPECT_EQ(Oblivion::dispositionTone(context.mData.mDisposition), Oblivion::DispositionTone::Hostile);
    EXPECT_TRUE(Oblivion::shouldInitiateCombat(context.mData.mAggression, context.mData.mDisposition));
    EXPECT_FALSE(Oblivion::shouldInitiateCombat(80, 90)); // friendly disposition: no attack
    EXPECT_TRUE(Oblivion::shouldInitiateCombat(106, 100)); // berserk: attacks everyone

    // Combat overrides the wander package.
    context.mTargetIsFoe = true;
    context.mHealthFraction = 1.f;
    Oblivion::AiDecision decision = Oblivion::evaluateAiBehavior(context);
    EXPECT_EQ(decision.mState, Oblivion::BehaviorState::Fight);
    EXPECT_TRUE(decision.mShouldInitiateCombat);

    // A wounded low-confidence actor flees.
    context.mData.mConfidence = 50;
    context.mHealthFraction = 0.3f;
    decision = Oblivion::evaluateAiBehavior(context);
    EXPECT_EQ(decision.mState, Oblivion::BehaviorState::Flee);

    // A 100-confidence actor never flees, but yields when almost dead.
    context.mData.mConfidence = 100;
    context.mHealthFraction = 0.05f;
    context.mData.mDisposition = 50;
    decision = Oblivion::evaluateAiBehavior(context);
    EXPECT_EQ(decision.mState, Oblivion::BehaviorState::Yield);

    // A healthy, non-hostile actor follows its package instead.
    Oblivion::AiContext wander;
    wander.mData.mDisposition = 80;
    wander.mData.mConfidence = 50;
    wander.mCurrentPackage = Oblivion::PackageType::Follow;
    const Oblivion::AiDecision wanderDecision = Oblivion::evaluateAiBehavior(wander);
    EXPECT_EQ(wanderDecision.mState, Oblivion::BehaviorState::Follow);
    EXPECT_FALSE(wanderDecision.mShouldInitiateCombat);

    // Witnessed crimes are reported by responsible NPCs only.
    Oblivion::AiContext crime;
    crime.mCrimeWitnessed = true;
    crime.mData.mResponsibility = 50;
    EXPECT_TRUE(Oblivion::evaluateAiBehavior(crime).mReportsCrime);
    crime.mData.mResponsibility = 10;
    EXPECT_FALSE(Oblivion::evaluateAiBehavior(crime).mReportsCrime);

    // Fame/infamy shift disposition.
    EXPECT_EQ(Oblivion::dispositionModifierFromFame(0), 0);
    EXPECT_EQ(Oblivion::dispositionModifierFromFame(67), 20); // capped at +20
    EXPECT_EQ(Oblivion::dispositionModifierFromInfamy(0, 100), 0);
    EXPECT_EQ(Oblivion::dispositionModifierFromInfamy(32, 100), -16);
    EXPECT_EQ(Oblivion::dispositionModifierFromInfamy(20, 25), -1);
}

// ---------- mechanics: game modes ----------

TEST(OblivionIntegration, gameModeSwitcherIntegratesMechanicsSystemWithEngineModes)
{
    RecordingGameModeSwitcher switcher;
    Oblivion::MechanicsSystem mechanics(&switcher);

    EXPECT_EQ(mechanics.getGameMode(), Oblivion::GameMode::Game);
    EXPECT_FALSE(mechanics.isInGameMode());

    ASSERT_TRUE(mechanics.requestGameMode(Oblivion::GameMode::Alchemy));
    EXPECT_EQ(mechanics.getGameMode(), Oblivion::GameMode::Alchemy);
    EXPECT_TRUE(mechanics.isInGameMode());
    ASSERT_EQ(switcher.mRequestedModes.size(), 1u);
    EXPECT_EQ(switcher.mRequestedModes.back(), Oblivion::GameMode::Alchemy);

    ASSERT_TRUE(mechanics.requestGameMode(Oblivion::GameMode::SpellMaking));
    EXPECT_EQ(mechanics.getGameMode(), Oblivion::GameMode::SpellMaking);

    // A rejecting switcher vetoes the mode change.
    switcher.mReject = true;
    EXPECT_FALSE(mechanics.requestGameMode(Oblivion::GameMode::Enchanting));
    EXPECT_EQ(mechanics.getGameMode(), Oblivion::GameMode::SpellMaking);

    // Leaving a mode always returns to Game.
    mechanics.leaveGameMode();
    EXPECT_EQ(mechanics.getGameMode(), Oblivion::GameMode::Game);
    EXPECT_FALSE(mechanics.isInGameMode());

    // Mode names match the ones the engine maps onto its UI modes.
    EXPECT_EQ(Oblivion::gameModeName(Oblivion::GameMode::Game), "Game");
    EXPECT_EQ(Oblivion::gameModeName(Oblivion::GameMode::Alchemy), "Alchemy");
    EXPECT_EQ(Oblivion::gameModeName(Oblivion::GameMode::SpellMaking), "Spell Making");
    EXPECT_EQ(Oblivion::gameModeName(Oblivion::GameMode::Enchanting), "Enchanting");

    // Headless mode (no switcher attached) accepts every transition.
    Oblivion::MechanicsSystem headless;
    EXPECT_TRUE(headless.requestGameMode(Oblivion::GameMode::Repair));
    EXPECT_EQ(headless.getGameMode(), Oblivion::GameMode::Repair);
}

// ---------- full pipeline: options -> check -> load -> gameplay ----------

TEST(OblivionIntegration, fullPipelineFromOptionsToLoadedWorldToGameplay)
{
    const std::filesystem::path dir = makeDataDir("full-pipeline");

    PluginOptions masterOptions;
    masterOptions.hedrVersion = ESM::VER_100;
    masterOptions.master = true;
    masterOptions.author = "Test Author";
    masterOptions.description = "Full pipeline master";
    writeFile(dir / "PipelineMaster.esm", buildPlugin(masterOptions));

    PluginOptions pluginOptions;
    pluginOptions.hedrVersion = ESM::VER_100;
    pluginOptions.master = false;
    pluginOptions.author = "Test Author";
    pluginOptions.masters = { { "PipelineMaster.esm", 0x0000beef } };
    writeFile(dir / "PipelineMod.esp", buildPlugin(pluginOptions));

    const std::vector<std::string> contentFiles = { "PipelineMaster.esm", "PipelineMod.esp" };

    // 1. Command line options as parsed by the engine at startup.
    const bpo::variables_map variables = parseWithOblivionOptions({ "--oblivion-verbose" });
    const Oblivion::Configuration config = Oblivion::loadConfiguration(variables);
    EXPECT_TRUE(config.mVerbose);
    EXPECT_TRUE(config.mEnable); // settings.cfg default

    // 2. The startup content check, over the whole content list.
    const Files::Collections collections({ dir });
    const Oblivion::ContentReport report = Oblivion::checkContentFiles(contentFiles, collections, config);
    EXPECT_EQ(report.mOblivionContentCount, 2u);
    expectNoErrors(report);

    // 3. The content loading pipeline loads both files in order.
    const Oblivion::ContentLoader loader;
    const Oblivion::ContentFile master = loader.load(dir / "PipelineMaster.esm");
    const Oblivion::ContentFile mod = loader.load(dir / "PipelineMod.esp");
    ASSERT_EQ(mod.mMasters.size(), 1u);
    EXPECT_EQ(mod.mMasters.front().mName, "PipelineMaster.esm");
    EXPECT_EQ(mod.mMasters.front().mSize, 0x0000beefu);
    EXPECT_TRUE(Oblivion::ContentLoader::findMissingMasters(mod, collections).empty());
    EXPECT_EQ(master.getTotalRecordCount(), 0u); // header-only synthetic master

    // 4. Gameplay: a character is created in the world defined by the content that was just loaded.
    Oblivion::ActorStats player(/*initialMaxHealth=*/50, /*initialMaxMagicka=*/25, /*initialMaxFatigue=*/100);
    player.setAttribute(Oblivion::Attribute::Strength, 30);
    player.setAttribute(Oblivion::Attribute::Endurance, 10);
    player.setAttribute(Oblivion::Attribute::Intelligence, 25);
    player.setAttribute(Oblivion::Attribute::Luck, 50);
    const Oblivion::ClassDefinition cls = makeCombatClass();
    player.setClass(cls);
    player.initializeFromClass();
    EXPECT_EQ(player.getSkill(Oblivion::Skill::Blade), 30);

    // The player picks up and identifies ingredients, brews a potion and drinks it.
    const Oblivion::AlchemyIngredient ingredientA{ { ESM4::EFI_REHE }, 0.2f };
    const Oblivion::AlchemyIngredient ingredientB{ { ESM4::EFI_REHE }, 0.4f };
    Oblivion::AlchemyApparatus apparatus;
    const std::vector<Oblivion::PotionEffect> potion = Oblivion::brewPotion({ ingredientA, ingredientB },
        player.getSkill(Oblivion::Skill::Alchemy), player.getAttribute(Oblivion::Attribute::Luck), apparatus);
    ASSERT_EQ(potion.size(), 1u);
    EXPECT_GE(potion.front().mMagnitude, 1);
    EXPECT_GE(potion.front().mDuration, 1);

    // The player raises a major skill until level up is available and levels up.
    for (int i = 0; i < 10; ++i)
        player.raiseSkill(Oblivion::Skill::Blade, 1);
    ASSERT_TRUE(player.isLevelUpReady());
    EXPECT_TRUE(player.levelUp({ Oblivion::Attribute::Strength, Oblivion::Attribute::Endurance,
        Oblivion::Attribute::Agility }));
    EXPECT_EQ(player.getLevel(), 2);
}
