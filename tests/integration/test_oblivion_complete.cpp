// Oblivion (TES4) support: complete integration test.
//
// This is the full-system test for Oblivion support: it exercises every Oblivion component
// together, in the order and in the way the engine uses them, as a complete session would:
//
//   1. launch configuration: the [Oblivion] section of files/settings-default.cfg and the
//      Settings system (components/settings), and the Oblivion command line options
//      registered on the engine's options description (components/oblivion/config.hpp,
//      wired into apps/openmw/options.cpp),
//   2. the startup content check over the configured content files
//      (components/oblivion/config.hpp, called from apps/openmw/main.cpp),
//   3. the content loading pipeline (components/oblivion/contentloader.hpp, used by
//      apps/openmw/mwworld/esmloader.cpp): format detection (components/esm/format.hpp),
//      per-file validation, load order and master dependency resolution, world space/cell
//      extraction from the group hierarchy, per-record-type statistics,
//   4. the generic TES4-family streaming reader the engine hands records to
//      (components/esm4/reader.hpp),
//   5. gameplay with the loaded content: character creation from a class definition, leveling,
//      Luck-modified effective skills, spellmaking, enchantment and soul gems, alchemy,
//      inventory/encumbrance, repair, AI behaviour and the game mode switcher that ties the
//      mechanics system into the engine's UI modes (components/oblivion/mechanics.hpp).
//
// The tests build synthetic Oblivion plugins (masters, plugins, a world space with exterior
// and interior cells) in a temporary directory, so they do not require any game data. They
// live at the path requested by the task brief; they are built as part of the
// components-tests target (see apps/components_tests/CMakeLists.txt), which is where all
// component and integration tests in this repository are built and run.

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

#include <algorithm>
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

    std::string issueSummary(const Oblivion::ContentReport& report)
    {
        std::ostringstream stream;
        for (const Oblivion::ConfigIssue& issue : report.mIssues)
        {
            const char* severity = issue.mSeverity == Oblivion::ConfigIssueSeverity::Error ? "error"
                : issue.mSeverity == Oblivion::ConfigIssueSeverity::Warning ? "warning"
                                                                             : "info";
            stream << "[" << severity << "] " << issue.mMessage << "\n";
        }
        return stream.str();
    }

    bool hasMessageContaining(const Oblivion::ContentReport& report, std::string_view needle)
    {
        for (const Oblivion::ConfigIssue& issue : report.mIssues)
            if (issue.mMessage.find(needle) != std::string::npos)
                return true;
        return false;
    }

    void expectNoErrors(const Oblivion::ContentReport& report)
    {
        EXPECT_FALSE(report.hasErrors()) << issueSummary(report);
    }

    void expectNoIssues(const Oblivion::ContentReport& report)
    {
        EXPECT_EQ(report.mIssues.size(), 0u) << issueSummary(report);
        EXPECT_FALSE(report.hasErrors());
        EXPECT_FALSE(report.hasWarnings());
    }

    // ---- minimal synthetic plugin builder (same shape as the other Oblivion tests) ----

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

    std::vector<char> makeFormIdSubRecord(std::uint32_t type, std::uint32_t formId)
    {
        BytesBuilder builder;
        builder.u32(formId);
        return makeSubRecord(type, builder.bytes);
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

    std::vector<char> makeGroup(std::uint32_t label, std::int32_t type, const std::vector<char>& content)
    {
        BytesBuilder builder;
        builder.u32(ESM::fourCC("GRUP"));
        builder.u32(static_cast<std::uint32_t>(20 + content.size()));
        builder.u32(label);
        builder.i32(type);
        builder.u32(0); // stamp
        builder.bytesFrom(content);
        return std::move(builder.bytes);
    }

    std::vector<char> concat(std::initializer_list<std::vector<char>> parts)
    {
        std::vector<char> result;
        for (const std::vector<char>& part : parts)
            result.insert(result.end(), part.begin(), part.end());
        return result;
    }

    void writeFile(const std::filesystem::path& path, const std::vector<char>& bytes)
    {
        std::ofstream stream(path, std::ios::binary);
        stream.exceptions(std::ofstream::failbit | std::ofstream::badbit);
        stream.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
    }

    std::filesystem::path makeDataDir(std::string_view name)
    {
        return TestingOpenMW::outputDirPath(std::filesystem::path("oblivion-complete") / std::string(name));
    }

    struct PluginOptions
    {
        std::uint32_t hedrVersion = ESM::VER_100;
        bool master = true; // set the Rec_ESM flag on the TES4 record
        std::int32_t numRecords = 0;
        std::uint32_t nextObjectId = 0x00000014;
        std::string author = "Bethesda Game Studios";
        std::string description = "Test plugin";
        std::vector<std::pair<std::string, std::uint64_t>> masters;
    };

    // Builds a synthetic Oblivion ESM/ESP with the given TES4 header and extra content
    // (raw, already serialized records/groups) appended after the TES4 record.
    std::vector<char> buildPlugin(const PluginOptions& options, const std::vector<char>& content = {})
    {
        BytesBuilder hedr;
        hedr.u32(options.hedrVersion); // float bits are just the raw 32-bit pattern
        hedr.i32(options.numRecords);
        hedr.u32(options.nextObjectId);
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
        const std::vector<char> tes4 = makeRecord(
            Files::Oblivion::REC_TES4, flags, /*formId=*/0, /*versionControlInfo=*/0, payload.bytes);

        std::vector<char> plugin = tes4;
        plugin.insert(plugin.end(), content.begin(), content.end());
        return plugin;
    }

    // A minimal Morrowind (TES3) file: only the signature bytes, which is all the
    // format detection (ESM::readFormat) and the Oblivion content check look at.
    std::vector<char> buildTes3File()
    {
        return { 'T', 'E', 'S', '3', 'T', 'E', 'S', '3' };
    }

    std::vector<char> makeWorldRecord(std::uint32_t formId, const std::string& editorId, const std::string& fullName,
        std::uint8_t worldFlags, std::uint32_t climate, std::uint32_t water)
    {
        BytesBuilder payload;
        payload.bytesFrom(makeStringSubRecord(ESM::fourCC("EDID"), editorId));
        payload.bytesFrom(makeStringSubRecord(ESM::fourCC("FULL"), fullName));
        payload.bytesFrom(makeSubRecord(ESM::fourCC("DATA"), { static_cast<char>(worldFlags) }));
        payload.bytesFrom(makeFormIdSubRecord(ESM::fourCC("CNAM"), climate));
        payload.bytesFrom(makeFormIdSubRecord(ESM::fourCC("NAM2"), water));
        payload.bytesFrom(makeStringSubRecord(ESM::fourCC("ICON"), "textures/terrain/tamriel.dds"));

        BytesBuilder dnam;
        const float land = -200.f;
        const float waterLevel = 0.f;
        const std::uint32_t landBits = *reinterpret_cast<const std::uint32_t*>(&land);
        const std::uint32_t waterBits = *reinterpret_cast<const std::uint32_t*>(&waterLevel);
        dnam.u32(landBits);
        dnam.u32(waterBits);
        payload.bytesFrom(makeSubRecord(ESM::fourCC("DNAM"), dnam.bytes));

        return makeRecord(Files::Oblivion::REC_WRLD, /*flags=*/0, formId, /*versionControlInfo=*/0, payload.bytes);
    }

    std::vector<char> makeExteriorCellRecord(std::uint32_t formId, const std::string& editorId, std::int32_t x,
        std::int32_t y, float waterHeight)
    {
        BytesBuilder payload;
        payload.bytesFrom(makeStringSubRecord(ESM::fourCC("EDID"), editorId));

        BytesBuilder data;
        data.u16(0); // exterior cell, not an interior cell
        payload.bytesFrom(makeSubRecord(ESM::fourCC("DATA"), data.bytes));

        BytesBuilder xlcl;
        xlcl.i32(x);
        xlcl.i32(y);
        xlcl.u32(0); // quad flags
        payload.bytesFrom(makeSubRecord(ESM::fourCC("XCLC"), xlcl.bytes));

        BytesBuilder xclw;
        const std::uint32_t waterBits = *reinterpret_cast<const std::uint32_t*>(&waterHeight);
        xclw.u32(waterBits);
        payload.bytesFrom(makeSubRecord(ESM::fourCC("XCLW"), xclw.bytes));

        return makeRecord(Files::Oblivion::REC_CELL, /*flags=*/0, formId, /*versionControlInfo=*/0, payload.bytes);
    }

    std::vector<char> makeInteriorCellRecord(std::uint32_t formId, const std::string& editorId,
        const std::string& fullName)
    {
        BytesBuilder payload;
        payload.bytesFrom(makeStringSubRecord(ESM::fourCC("EDID"), editorId));
        payload.bytesFrom(makeStringSubRecord(ESM::fourCC("FULL"), fullName));

        BytesBuilder data;
        data.u16(Oblivion::kCellFlagInterior);
        payload.bytesFrom(makeSubRecord(ESM::fourCC("DATA"), data.bytes));

        return makeRecord(Files::Oblivion::REC_CELL, /*flags=*/0, formId, /*versionControlInfo=*/0, payload.bytes);
    }

    std::vector<char> makeReferenceRecord(std::uint32_t formId)
    {
        return makeRecord(Files::Oblivion::REC_REFR, /*flags=*/0, formId, /*versionControlInfo=*/0,
            std::vector<char>{});
    }

    // A small but representative Oblivion world: a WRLD record plus an exterior cell with a
    // reference and an interior cell, wrapped in the group hierarchy the game uses.
    std::vector<char> buildWorldCellContent()
    {
        const std::vector<char> wrld = makeWorldRecord(0x00000010, "Tamriel", "Tamriel",
            Oblivion::kWorldFlagOblivion, /*climate=*/0x00000040, /*water=*/0x00000041);
        const std::vector<char> wrldGroup = makeGroup(ESM::fourCC("WRLD"), Files::Oblivion::Grp_RecordType, wrld);

        const std::vector<char> cell = makeExteriorCellRecord(0x00000020, "TamrielExterior", -7, 3, 4.5f);
        const std::vector<char> refr = makeReferenceRecord(0x00000050);
        const std::vector<char> cellChild = makeGroup(0x00000020, Files::Oblivion::Grp_CellChild, refr);
        const std::vector<char> subBlock = makeGroup(0, Files::Oblivion::Grp_ExteriorCellSubBlock,
            concat({ cell, cellChild }));
        const std::vector<char> block = makeGroup(0, Files::Oblivion::Grp_ExteriorCellBlock, subBlock);
        const std::vector<char> worldChild = makeGroup(0x00000010, Files::Oblivion::Grp_WorldChild, block);

        const std::vector<char> interiorCell = makeInteriorCellRecord(0x00000030, "Prison", "Imperial City Prison");
        const std::vector<char> interiorSubBlock = makeGroup(0, Files::Oblivion::Grp_InteriorCellSubBlock, interiorCell);
        const std::vector<char> interiorBlock = makeGroup(0, Files::Oblivion::Grp_InteriorCellBlock, interiorSubBlock);
        const std::vector<char> interiorGroup = makeGroup(ESM::fourCC("CELL"), Files::Oblivion::Grp_RecordType, interiorBlock);

        return concat({ wrldGroup, worldChild, interiorGroup });
    }

    // Write the standard two-file test fixture (a master with a small world, and a plugin
    // that depends on it) into a fresh temporary data directory and return its path.
    std::filesystem::path writeStandardFixture(std::string_view dirName, PluginOptions masterOptions = {},
        PluginOptions pluginOptions = {})
    {
        const std::filesystem::path dir = makeDataDir(dirName);

        masterOptions.master = true;
        if (masterOptions.author.empty())
            masterOptions.author = "Bethesda Game Studios";
        writeFile(dir / "CompleteMaster.esm", buildPlugin(masterOptions, buildWorldCellContent()));

        pluginOptions.master = false;
        pluginOptions.masters = { { "CompleteMaster.esm", 0x0000abcd } };
        writeFile(dir / "CompleteMod.esp", buildPlugin(pluginOptions));

        return dir;
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

    const std::unordered_map<std::uint32_t, Oblivion::ItemDefinition>& testItemDefinitions()
    {
        static const std::unordered_map<std::uint32_t, Oblivion::ItemDefinition> definitions{
            { 0x11, { 0x11, Oblivion::ItemType::Weapon, "Iron Dagger", 4.f, 25, 50, false } },
            { 0x12, { 0x12, Oblivion::ItemType::Armor, "Iron Helmet", 5.f, 100, 100, false } },
            { 0x13, { 0x13, Oblivion::ItemType::Ingredient, "Apple", 0.1f, 1, 0, false } },
            { 0x14, { 0x14, Oblivion::ItemType::Potion, "Potion of Restore Health", 0.5f, 40, 0, false } },
        };
        return definitions;
    }
}

// ---------- stage 1: launch configuration (settings + command line) ----------

TEST(OblivionComplete, engineShipsConsistentOblivionDefaults)
{
    // files/settings-default.cfg must ship the [Oblivion] section; components-tests' main.cpp
    // loads it into Settings::Manager before any test runs, but verify the file contents
    // directly as well so a missing section fails this test, not a distant one.
    const std::filesystem::path settingsDefaultPath = std::filesystem::path{ OPENMW_PROJECT_SOURCE_DIR } / "files"
        / "settings-default.cfg";
    std::ifstream stream(settingsDefaultPath);
    ASSERT_TRUE(stream.is_open()) << "failed to open " << settingsDefaultPath;
    const std::string contents((std::istreambuf_iterator<char>(stream)), std::istreambuf_iterator<char>());
    const std::size_t section = contents.find("[Oblivion]");
    ASSERT_NE(section, std::string::npos);
    EXPECT_NE(contents.find("enable = true", section), std::string::npos);
    EXPECT_NE(contents.find("validate content = true", section), std::string::npos);
    EXPECT_NE(contents.find("strict load order = false", section), std::string::npos);
    EXPECT_NE(contents.find("verbose content loading = false", section), std::string::npos);

    // A fresh Values instance resolves the same documented defaults.
    Settings::Index index;
    Settings::Values values(index);
    EXPECT_TRUE(values.mOblivion.mEnable.get());
    EXPECT_TRUE(values.mOblivion.mValidateContent.get());
    EXPECT_FALSE(values.mOblivion.mStrictLoadOrder.get());
    EXPECT_FALSE(values.mOblivion.mVerboseContentLoading.get());

    // The built-in Configuration defaults agree with the documented settings defaults.
    const Oblivion::Configuration config;
    EXPECT_TRUE(config.mEnable);
    EXPECT_TRUE(config.mValidate);
    EXPECT_FALSE(config.mStrictLoadOrder);
    EXPECT_FALSE(config.mVerbose);
    EXPECT_FALSE(config.mForced);

    // An empty command line (i.e. all values from openmw.cfg/settings.cfg, both absent here)
    // produces the same effective configuration through the engine's options plumbing.
    const bpo::variables_map variables = parseWithOblivionOptions({});
    const Oblivion::Configuration effective = Oblivion::loadConfiguration(variables);
    EXPECT_TRUE(effective.mEnable);
    EXPECT_TRUE(effective.mValidate);
    EXPECT_FALSE(effective.mForced);
}

// ---------- stage 2-4: the complete launch pipeline over a synthetic world ----------

TEST(OblivionComplete, fullLaunchPipelineLoadsOblivionWorldFromDisk)
{
    const std::filesystem::path dir = writeStandardFixture("launch-pipeline");

    const std::vector<std::string> contentFiles = { "CompleteMaster.esm", "CompleteMod.esp" };

    // 1. Command line options as parsed by the engine at startup (--oblivion forces the mode).
    const bpo::variables_map variables = parseWithOblivionOptions({ "--oblivion" });
    const Oblivion::Configuration config = Oblivion::loadConfiguration(variables);
    ASSERT_TRUE(config.mForced);
    ASSERT_TRUE(config.mValidate);

    // 2. The startup content check, over the whole content list.
    const Files::Collections collections({ dir });
    const Oblivion::ContentReport report = Oblivion::checkContentFiles(contentFiles, collections, config);
    EXPECT_EQ(report.mOblivionContentCount, 2u);
    expectNoErrors(report);

    // 3. Format detection agrees on what the files are.
    for (const std::string& file : contentFiles)
    {
        const std::filesystem::path path = dir / file;
        auto stream = Files::openBinaryInputFileStream(path);
        ASSERT_TRUE(stream);
        EXPECT_EQ(ESM::Format::Tes4, ESM::readFormat(*stream));
        ASSERT_TRUE(Oblivion::ContentLoader::isOblivionPlugin(path));
    }

    // 4. The content loading pipeline loads the master, interprets its group hierarchy and
    //    hands it to the engine's stores in load order.
    const Oblivion::ContentLoader loader;

    const Oblivion::ContentFile master = loader.load(dir / "CompleteMaster.esm");
    EXPECT_TRUE(master.mMaster);
    EXPECT_EQ(master.mHedrVersion, ESM::VER_100);
    EXPECT_EQ(master.mAuthor, "Bethesda Game Studios");
    EXPECT_EQ(master.mNextObjectId, 0x00000014u);
    EXPECT_EQ(master.getRecordCount(Files::Oblivion::REC_WRLD), 1u);
    EXPECT_EQ(master.getRecordCount(Files::Oblivion::REC_CELL), 2u);
    EXPECT_EQ(master.getRecordCount(Files::Oblivion::REC_REFR), 1u);
    EXPECT_EQ(master.getTotalRecordCount(), 4u);

    ASSERT_EQ(master.mWorlds.size(), 1u);
    const Oblivion::WorldData& tamriel = master.mWorlds.front();
    EXPECT_EQ(tamriel.mFormId, 0x00000010u);
    EXPECT_EQ(tamriel.mEditorId, "Tamriel");
    EXPECT_EQ(tamriel.mFullName, "Tamriel");
    EXPECT_EQ(tamriel.mWorldFlags, Oblivion::kWorldFlagOblivion);
    EXPECT_EQ(tamriel.mClimate, 0x00000040u);
    EXPECT_EQ(tamriel.mWater, 0x00000041u);
    EXPECT_EQ(tamriel.mMapFile, "textures/terrain/tamriel.dds");
    EXPECT_FLOAT_EQ(tamriel.mLandLevel, -200.f);
    EXPECT_FLOAT_EQ(tamriel.mWaterLevel, 0.f);
    EXPECT_EQ(tamriel.mCells, (std::vector<std::uint32_t>{ 0x00000020u }));

    ASSERT_EQ(master.mCells.size(), 2u);
    const Oblivion::CellData* exterior = master.findCell(0x00000020u);
    ASSERT_NE(exterior, nullptr);
    EXPECT_TRUE(exterior->isExterior());
    EXPECT_EQ(exterior->mEditorId, "TamrielExterior");
    EXPECT_EQ(exterior->mWorld, 0x00000010u);
    EXPECT_EQ(exterior->mX, -7);
    EXPECT_EQ(exterior->mY, 3);
    EXPECT_FLOAT_EQ(exterior->mWaterHeight, 4.5f);

    const Oblivion::CellData* interior = master.findCell(0x00000030u);
    ASSERT_NE(interior, nullptr);
    EXPECT_FALSE(interior->isExterior());
    EXPECT_EQ(interior->mFullName, "Imperial City Prison");
    EXPECT_EQ(interior->mWorld, 0u);

    // The world's exterior cell list resolves through the public query API.
    const std::vector<const Oblivion::CellData*> tamrielCells = master.getCells(0x00000010u);
    ASSERT_EQ(tamrielCells.size(), 1u);
    EXPECT_EQ(tamrielCells.front()->mFormId, 0x00000020u);

    const Oblivion::ContentFile mod = loader.load(dir / "CompleteMod.esp");
    EXPECT_FALSE(mod.mMaster);
    ASSERT_EQ(mod.mMasters.size(), 1u);
    EXPECT_EQ(mod.mMasters.front().mName, "CompleteMaster.esm");
    EXPECT_EQ(mod.mMasters.front().mSize, 0x0000abcdu);
    EXPECT_TRUE(Oblivion::ContentLoader::findMissingMasters(mod, collections).empty());
    EXPECT_TRUE(Oblivion::ContentLoader::findMissingMasters(mod, contentFiles).empty());

    // 5. The streaming TES4-family reader the engine uses reads the same two files.
    {
        const std::filesystem::path masterPath = dir / "CompleteMaster.esm";
        ESM4::Reader esm4Master(
            Files::openBinaryInputFileStream(masterPath), masterPath, /*vfs=*/nullptr, /*encoder=*/nullptr);
        EXPECT_EQ(esm4Master.gameFormat(), ESM::GameFormat::TES4);
        EXPECT_EQ(esm4Master.esmVersion(), ESM::VER_100);
        EXPECT_TRUE(esm4Master.getGameFiles().empty());
        EXPECT_EQ(esm4Master.getAuthor(), "Bethesda Game Studios");
    }
    {
        const std::filesystem::path modPath = dir / "CompleteMod.esp";
        ESM4::Reader esm4Mod(
            Files::openBinaryInputFileStream(modPath), modPath, /*vfs=*/nullptr, /*encoder=*/nullptr);
        EXPECT_EQ(esm4Mod.gameFormat(), ESM::GameFormat::TES4);
        ASSERT_EQ(esm4Mod.getGameFiles().size(), 1u);
        EXPECT_EQ(esm4Mod.getGameFiles().front().name, "CompleteMaster.esm");
    }
}

// ---------- stage 2-4: a three-file load order, end to end ----------

TEST(OblivionComplete, threeFileLoadOrderChainResolvesAcrossEntireList)
{
    const std::filesystem::path dir = makeDataDir("three-file-chain");

    PluginOptions oblivionMaster;
    oblivionMaster.hedrVersion = ESM::VER_100;
    oblivionMaster.master = true;
    oblivionMaster.author = "Bethesda";
    oblivionMaster.description = "Oblivion master";
    writeFile(dir / "Oblivion.esm", buildPlugin(oblivionMaster, buildWorldCellContent()));

    PluginOptions updateMaster;
    updateMaster.hedrVersion = ESM::VER_100;
    updateMaster.master = true;
    updateMaster.author = "Bethesda";
    updateMaster.masters = { { "Oblivion.esm", 123 } };
    writeFile(dir / "Update.esm", buildPlugin(updateMaster));

    PluginOptions mod;
    mod.hedrVersion = ESM::VER_080; // pre-1.1 plugins are valid Oblivion content
    mod.master = false;
    mod.masters = { { "Oblivion.esm", 111 }, { "Update.esm", 222 } };
    writeFile(dir / "Mod.esp", buildPlugin(mod));

    const std::vector<std::string> contentFiles = { "Oblivion.esm", "Update.esm", "Mod.esp" };
    const Files::Collections collections({ dir });
    const Oblivion::Configuration config;

    // The startup check accepts the whole chain in dependency order.
    const Oblivion::ContentReport report = Oblivion::checkContentFiles(contentFiles, collections, config);
    EXPECT_EQ(report.mOblivionContentCount, 3u);
    expectNoIssues(report);

    // Every file loads and every dependency resolves against the configured list.
    const Oblivion::ContentLoader loader;
    const Oblivion::ContentFile update = loader.load(dir / "Update.esm");
    const Oblivion::ContentFile modContent = loader.load(dir / "Mod.esp");
    ASSERT_EQ(update.mMasters.size(), 1u);
    ASSERT_EQ(modContent.mMasters.size(), 2u);
    EXPECT_EQ(modContent.mMasters[0].mName, "Oblivion.esm");
    EXPECT_EQ(modContent.mMasters[0].mSize, 111u);
    EXPECT_EQ(modContent.mMasters[1].mName, "Update.esm");
    EXPECT_EQ(modContent.mMasters[1].mSize, 222u);
    EXPECT_TRUE(Oblivion::ContentLoader::findMissingMasters(modContent, contentFiles).empty());
    EXPECT_TRUE(Oblivion::ContentLoader::findMissingMasters(update, contentFiles).empty());

    // The master carries the world data; the derived files stay header-only.
    const Oblivion::ContentFile oblivion = loader.load(dir / "Oblivion.esm");
    EXPECT_EQ(oblivion.getRecordCount(Files::Oblivion::REC_WRLD), 1u);
    EXPECT_EQ(update.getTotalRecordCount(), 0u);
    EXPECT_EQ(modContent.getTotalRecordCount(), 0u);

    // Listing a plugin before its master breaks both dependents; strict mode turns it fatal.
    const std::vector<std::string> reversed = { "Update.esm", "Oblivion.esm", "Mod.esp" };
    const Oblivion::ContentReport reversedReport = Oblivion::checkContentFiles(reversed, collections, config);
    EXPECT_EQ(reversedReport.mOblivionContentCount, 3u);
    EXPECT_FALSE(reversedReport.hasErrors()) << issueSummary(reversedReport);
    EXPECT_TRUE(reversedReport.hasWarnings());
    EXPECT_TRUE(hasMessageContaining(reversedReport, "is listed after")) << issueSummary(reversedReport);

    Oblivion::Configuration strict;
    strict.mStrictLoadOrder = true;
    const Oblivion::ContentReport strictReport = Oblivion::checkContentFiles(reversed, collections, strict);
    EXPECT_TRUE(strictReport.hasErrors()) << issueSummary(strictReport);
}

// ---------- stage 1-4: mixed Morrowind and Oblivion content in one session ----------

TEST(OblivionComplete, mixedMorrowindAndOblivionContentSession)
{
    const std::filesystem::path dir = makeDataDir("mixed-content");

    writeFile(dir / "Morrowind.esm", buildTes3File());

    PluginOptions masterOptions;
    masterOptions.hedrVersion = ESM::VER_100;
    masterOptions.master = true;
    writeFile(dir / "RealOblivionMaster.esm", buildPlugin(masterOptions, buildWorldCellContent()));

    const std::vector<std::string> contentFiles = { "Morrowind.esm", "RealOblivionMaster.esm" };
    const Files::Collections collections({ dir });

    // The startup check only processes the TES4-family file; Morrowind content is passed
    // through to the Morrowind pipeline untouched.
    const Oblivion::Configuration config;
    const Oblivion::ContentReport report = Oblivion::checkContentFiles(contentFiles, collections, config);
    EXPECT_EQ(report.mOblivionContentCount, 1u);
    expectNoErrors(report);
    EXPECT_TRUE(hasMessageContaining(report, "Morrowind")) << issueSummary(report);

    // Format detection tells the two apart, and the Oblivion loader handles only its own.
    auto tes3Stream = Files::openBinaryInputFileStream(dir / "Morrowind.esm");
    ASSERT_TRUE(tes3Stream);
    EXPECT_EQ(ESM::Format::Tes3, ESM::readFormat(*tes3Stream));
    EXPECT_FALSE(Oblivion::ContentLoader::isOblivionPlugin(dir / "Morrowind.esm"));
    EXPECT_TRUE(Oblivion::ContentLoader::isOblivionPlugin(dir / "RealOblivionMaster.esm"));

    const Oblivion::ContentFile oblivion = Oblivion::ContentLoader().load(dir / "RealOblivionMaster.esm");
    EXPECT_EQ(oblivion.mWorlds.size(), 1u);

    // Forcing Oblivion mode makes configured Morrowind content fatal, as configured.
    Oblivion::Configuration forced;
    forced.mForced = true;
    forced.mValidate = true;
    const Oblivion::ContentReport forcedReport = Oblivion::checkContentFiles(contentFiles, collections, forced);
    EXPECT_TRUE(forcedReport.hasErrors()) << issueSummary(forcedReport);
}

// ---------- stage 2-4: invalid content fails at the right pipeline stage ----------

TEST(OblivionComplete, invalidContentIsRejectedBeforeItReachesTheReader)
{
    const std::filesystem::path dir = makeDataDir("invalid-content");

    // Stage 2 (startup check): a content file that does not exist on disk is an error, and
    // the loading pipeline is never asked to open it.
    {
        const Files::Collections collections({ dir });
        const Oblivion::Configuration config;
        const std::vector<std::string> contentFiles = { "DoesNotExist.esm" };
        const Oblivion::ContentReport report = Oblivion::checkContentFiles(contentFiles, collections, config);
        EXPECT_EQ(report.mOblivionContentCount, 0u);
        ASSERT_TRUE(report.hasErrors()) << issueSummary(report);
        EXPECT_TRUE(hasMessageContaining(report, "does not exist")) << issueSummary(report);
    }

    // Stage 2/3 (forced mode): a TES4-signature plugin from another game of the family is
    // rejected at the check and refused by the loader.
    {
        PluginOptions fo3Options;
        fo3Options.hedrVersion = ESM::VER_094; // Fallout 3: TES4 signature, not an Oblivion HEDR version
        fo3Options.master = true;
        writeFile(dir / "Fallout3.esm", buildPlugin(fo3Options));

        const Files::Collections collections({ dir });
        Oblivion::Configuration forced;
        forced.mForced = true;
        forced.mValidate = true;
        const std::vector<std::string> contentFiles = { "Fallout3.esm" };
        const Oblivion::ContentReport report = Oblivion::checkContentFiles(contentFiles, collections, forced);
        ASSERT_TRUE(report.hasErrors()) << issueSummary(report);

        EXPECT_FALSE(Oblivion::ContentLoader::isOblivionPlugin(dir / "Fallout3.esm"));
        EXPECT_THROW(Oblivion::ContentLoader::validate(dir / "Fallout3.esm"), std::runtime_error);
        EXPECT_THROW(Oblivion::ContentLoader().load(dir / "Fallout3.esm"), std::runtime_error);
    }

    // Stage 3/4 (load time): a plugin whose header parses but whose body is corrupt passes
    // the startup check and fails when the engine actually loads it — an exception, not
    // silent data corruption.
    {
        PluginOptions options;
        options.hedrVersion = ESM::VER_100;
        options.master = true;

        BytesBuilder corruptGroup;
        corruptGroup.u32(ESM::fourCC("GRUP"));
        corruptGroup.u32(10); // groupSize < sizeof(GroupHeader)
        corruptGroup.u32(0);
        corruptGroup.i32(Files::Oblivion::Grp_RecordType);
        corruptGroup.u32(0);
        corruptGroup.u32(0);
        writeFile(dir / "Corrupt.esm", buildPlugin(options, corruptGroup.bytes));

        const Files::Collections collections({ dir });
        const Oblivion::Configuration config;
        const std::vector<std::string> contentFiles = { "Corrupt.esm" };
        const Oblivion::ContentReport report = Oblivion::checkContentFiles(contentFiles, collections, config);
        EXPECT_EQ(report.mOblivionContentCount, 1u);
        expectNoErrors(report); // the header is fine

        EXPECT_THROW(Oblivion::ContentLoader().load(dir / "Corrupt.esm"), std::runtime_error);
    }
}

// ---------- stage 5: gameplay — full session with the loaded content ----------

TEST(OblivionComplete, completeSessionPlaythroughWithLoadedContent)
{
    const std::filesystem::path dir = writeStandardFixture("playthrough");
    const std::vector<std::string> contentFiles = { "CompleteMaster.esm", "CompleteMod.esp" };

    // Bring the world up exactly as the engine would, then play in it.
    const Files::Collections collections({ dir });
    const Oblivion::Configuration config;
    const Oblivion::ContentReport report = Oblivion::checkContentFiles(contentFiles, collections, config);
    EXPECT_EQ(report.mOblivionContentCount, 2u);
    expectNoErrors(report);

    const Oblivion::ContentLoader loader;
    const Oblivion::ContentFile master = loader.load(dir / "CompleteMaster.esm");
    ASSERT_EQ(master.mCells.size(), 2u);

    // --- character creation from an Oblivion class definition ---
    const Oblivion::ClassDefinition cls = makeCombatClass();

    Oblivion::ActorStats player(/*initialMaxHealth=*/50, /*initialMaxMagicka=*/25, /*initialMaxFatigue=*/100);
    player.setAttribute(Oblivion::Attribute::Strength, 30);
    player.setAttribute(Oblivion::Attribute::Endurance, 10);
    player.setAttribute(Oblivion::Attribute::Intelligence, 25);
    player.setAttribute(Oblivion::Attribute::Luck, 50);
    player.setClass(cls);
    player.initializeFromClass();

    // Major skills of a Combat specialization start at 30 (25 + 5); the rest at 5.
    EXPECT_EQ(player.getSkill(Oblivion::Skill::Blade), 30);
    EXPECT_EQ(player.getSkill(Oblivion::Skill::Alchemy), 5);
    EXPECT_EQ(player.getMaxEncumbrance(), 150);
    EXPECT_EQ(player.getMaxMagicka(), 50); // 25 (initial) + 25 (Intelligence)
    EXPECT_EQ(player.getMaxFatigue(), 140); // 100 (initial) + 30 + 10

    // --- pick up ingredients, brew a potion, drink it ---
    const Oblivion::AlchemyIngredient apple{ { ESM4::EFI_REHE, ESM4::EFI_DGHE }, 0.5f };
    const Oblivion::AlchemyIngredient pear{ { ESM4::EFI_REHE, ESM4::EFI_DGHE }, 0.3f };

    Oblivion::Inventory inventory(testItemDefinitions());
    inventory.addItem(0x13, 2); // apples
    inventory.addItem(0x11, 1, /*stolen=*/true); // a lifted dagger
    EXPECT_EQ(inventory.getItemCount(0x13), 2);

    Oblivion::AlchemyApparatus apparatus;
    const std::vector<Oblivion::PotionEffect> potion = Oblivion::brewPotion({ apple, pear },
        player.getSkill(Oblivion::Skill::Alchemy), player.getAttribute(Oblivion::Attribute::Luck), apparatus);
    ASSERT_EQ(potion.size(), 2u); // both effects are common to both ingredients
    EXPECT_FALSE(Oblivion::isPoison(potion)); // Restore/Damage Health pair includes a positive effect
    inventory.addItem(0x14, 1);
    EXPECT_EQ(inventory.getItemCount(0x14), 1);

    // The stolen dagger is laundered by the enchanting service.
    inventory.applyEnchantment(0x11);
    inventory.clearStolenFlag(0x11);

    // --- cast a custom flame spell in the loaded world ---
    Oblivion::Spell flameBolt;
    flameBolt.mName = "Flame Bolt";
    flameBolt.mEffects = { makeEffect(ESM4::EFI_FIDG, Oblivion::SpellRange::Touch, 5, 5, 3) };

    const Oblivion::SchoolSkills skills = Oblivion::effectiveSchoolSkills(player);
    const std::int32_t spellSkill = skills[static_cast<std::size_t>(Oblivion::MagicSchool::Destruction)];
    const float spellCost = Oblivion::spellMagickaCost(flameBolt, skills);
    ASSERT_GT(spellCost, 0.f);
    EXPECT_GT(spellSkill, 0);
    EXPECT_TRUE(Oblivion::canCastSpell(flameBolt, skills, player.getMaxMagicka()));
    EXPECT_FALSE(Oblivion::canCastSpell(flameBolt, skills, 0));

    // --- an NPC of the loaded world evaluates the player, then fights ---
    Oblivion::AiContext context;
    context.mData.mAggression = 80;
    context.mData.mConfidence = 50;
    context.mData.mResponsibility = 50;
    context.mData.mDisposition = 10; // hated
    context.mTargetVisible = true;
    context.mTargetIsFoe = true;
    context.mHealthFraction = 1.f;
    context.mCurrentPackage = Oblivion::PackageType::Wander;

    const Oblivion::AiDecision decision = Oblivion::evaluateAiBehavior(context);
    EXPECT_EQ(decision.mState, Oblivion::BehaviorState::Fight);
    EXPECT_TRUE(decision.mShouldInitiateCombat);
    EXPECT_TRUE(Oblivion::shouldInitiateCombat(context.mData.mAggression, context.mData.mDisposition));

    // --- the player raises Blade until a level up is available and levels up ---
    for (int i = 0; i < 10; ++i)
        player.raiseSkill(Oblivion::Skill::Blade, 1);
    EXPECT_EQ(player.getSkill(Oblivion::Skill::Blade), 40);
    ASSERT_TRUE(player.isLevelUpReady());

    const std::array<Oblivion::Attribute, 3> chosen = { Oblivion::Attribute::Strength,
        Oblivion::Attribute::Endurance, Oblivion::Attribute::Agility };
    ASSERT_TRUE(player.levelUp(chosen));
    EXPECT_EQ(player.getLevel(), 2);
    EXPECT_EQ(player.getAttribute(Oblivion::Attribute::Strength), 35); // 30 + 5 (10 increases)
    EXPECT_EQ(player.getAttribute(Oblivion::Attribute::Endurance), 11); // 10 + 1 (no increases)
    // Health: 50 + 10% of the updated Endurance.
    EXPECT_EQ(player.getMaxHealth(), 51);

    // --- the master list the session rolled with is what the engine hands to its stores ---
    EXPECT_EQ(master.findWorld(0x00000010u), &master.mWorlds.front());
    EXPECT_EQ(master.findCell(0x00000030u)->mFullName, "Imperial City Prison");
}

// ---------- stage 5: derived attributes, inventory and equipment stay consistent ----------

TEST(OblivionComplete, derivedAttributesTrackInventoryEncumbranceAndMagicka)
{
    const Oblivion::ClassDefinition cls = makeCombatClass();

    Oblivion::ActorStats stats(0, 20, 0);
    stats.setAttribute(Oblivion::Attribute::Strength, 40);
    stats.setAttribute(Oblivion::Attribute::Endurance, 20);
    stats.setAttribute(Oblivion::Attribute::Agility, 15);
    stats.setAttribute(Oblivion::Attribute::Willpower, 10);
    stats.setAttribute(Oblivion::Attribute::Intelligence, 30);
    stats.setClass(cls);
    stats.initializeFromClass();

    // Derived values follow the base attributes (https://en.uesp.net/wiki/Oblivion:Attributes).
    EXPECT_EQ(stats.getMaxEncumbrance(), 200); // 5 per point of Strength
    EXPECT_EQ(stats.getMaxMagicka(), 50); // 20 (initial) + 30 (Intelligence)
    EXPECT_EQ(stats.getMaxFatigue(), 85); // 40 + 20 + 15 + 10

    // Inventory weight and value are computed from the same definition table the
    // content pipeline would load; encumbrance constrains what can be carried.
    Oblivion::Inventory inventory(testItemDefinitions());
    inventory.addItem(0x11, 10); // 10 x Iron Dagger, 4.f each
    inventory.addItem(0x12, 4); // 4 x Iron Helmet, 5.f each
    EXPECT_FLOAT_EQ(inventory.getTotalWeight(), 60.f);
    EXPECT_EQ(inventory.getTotalValue(), 10 * 25 + 4 * 100);
    EXPECT_TRUE(inventory.canCarry(static_cast<float>(stats.getMaxEncumbrance())));
    EXPECT_FLOAT_EQ(inventory.getFreeCapacity(static_cast<float>(stats.getMaxEncumbrance())), 140.f);

    ASSERT_TRUE(inventory.removeItem(0x11, 6));
    EXPECT_EQ(inventory.getItemCount(0x11), 4);
    EXPECT_FALSE(inventory.removeItem(0x11, 5)); // only 4 left
    EXPECT_FLOAT_EQ(inventory.getTotalWeight(), 36.f);

    // Repairing a pristine item restores nothing; the Armorer path agrees on the numbers.
    EXPECT_EQ(inventory.repairItem(0x12, 50), 0);
    EXPECT_EQ(Oblivion::armorerHealthRepaired(50), 53); // Armorer + 3
    EXPECT_FLOAT_EQ(Oblivion::maxRepairCondition(74), 1.0f);
    EXPECT_FLOAT_EQ(Oblivion::maxRepairCondition(75), 1.25f); // Expert Armorer over-repairs
    EXPECT_FALSE(Oblivion::canRepairMagicItems(49));
    EXPECT_TRUE(Oblivion::canRepairMagicItems(50)); // Journeyman Armorer repairs magic items
    EXPECT_FALSE(Oblivion::repairHammerBreaks(100, 0.f)); // Master Armorer never breaks hammers
    EXPECT_TRUE(Oblivion::repairHammerBreaks(0, 0.f)); // anyone else with roll 0 breaks

    // The magicka pool is what the player casts from; the derived attributes scale with it.
    EXPECT_EQ(stats.getMaxMagicka(), 50);
    stats.setAttribute(Oblivion::Attribute::Intelligence, 50);
    EXPECT_EQ(stats.getMaxMagicka(), 70);
}

// ---------- stage 5: magical economy — spellmaking, enchanting, alchemy ----------

TEST(OblivionComplete, magicalEconomyFromSpellmakingToEnchantingToAlchemy)
{
    Oblivion::ActorStats caster(0, 100, 0);
    caster.setAttribute(Oblivion::Attribute::Intelligence, 40);
    caster.setAttribute(Oblivion::Attribute::Luck, 50); // no Luck modifier
    caster.setSkill(Oblivion::Skill::Destruction, 80); // a skilled caster pays less than the base cost

    const Oblivion::SchoolSkills skills = Oblivion::effectiveSchoolSkills(caster);

    // A custom spell: Fire Damage, Touch, magnitude 5, duration 3.
    Oblivion::Spell flameBolt;
    flameBolt.mName = "Flame Bolt";
    flameBolt.mEffects = { makeEffect(ESM4::EFI_FIDG, Oblivion::SpellRange::Touch, 5, 5, 3) };

    const float cost = Oblivion::spellMagickaCost(flameBolt, skills);
    const float baseCost = Oblivion::spellBaseMagickaCost(flameBolt);
    ASSERT_GT(baseCost, 0.f);
    ASSERT_GT(cost, 0.f);
    EXPECT_LT(cost, baseCost); // a 30 Destruction skill discounts the cost

    const Oblivion::SpellRequirement requirement = Oblivion::spellSkillRequirement(flameBolt);
    EXPECT_EQ(requirement.mSchool, Oblivion::MagicSchool::Destruction);
    EXPECT_EQ(requirement.mRequiredSkill, 0); // base cost < 26 -> novice spell

    EXPECT_TRUE(Oblivion::canCastSpell(flameBolt, skills, caster.getMaxMagicka()));
    EXPECT_FALSE(Oblivion::canCastSpell(flameBolt, skills, 0));
    EXPECT_EQ(Oblivion::spellGoldCost(flameBolt, skills),
        static_cast<std::int32_t>(std::floor(3.f * cost))); // thrice the magicka cost

    // A cheaper caster pays more magicka for the same spell.
    Oblivion::ActorStats novice(0, 100, 0);
    novice.setAttribute(Oblivion::Attribute::Luck, 50);
    novice.setSkill(Oblivion::Skill::Destruction, 0);
    const Oblivion::SchoolSkills noviceSkills = Oblivion::effectiveSchoolSkills(novice);
    EXPECT_GT(Oblivion::spellMagickaCost(flameBolt, noviceSkills), cost);

    // --- constant-effect enchantments scale with soul gem level ---
    const std::int32_t feather1 = Oblivion::constantEffectEnchantmentMagnitude(ESM4::EFI_FTHR, 1);
    const std::int32_t feather5 = Oblivion::constantEffectEnchantmentMagnitude(ESM4::EFI_FTHR, 5);
    EXPECT_EQ(feather1, 14);
    EXPECT_EQ(feather5, 50);
    EXPECT_GT(feather5, feather1);
    EXPECT_EQ(Oblivion::constantEffectEnchantmentValueBonus(ESM4::EFI_FTHR, 5), 1250); // 50 * 25 barter factor
    EXPECT_EQ(Oblivion::soulGemCapacity(Oblivion::SoulGemType::Petty), 150);
    EXPECT_EQ(Oblivion::soulGemCapacity(Oblivion::SoulGemType::Grand), 1600);
    EXPECT_EQ(Oblivion::soulGemCapacity(Oblivion::SoulGemType::Black), 1600); // black gems hold grand capacity
    EXPECT_EQ(Oblivion::soulGemLevel(Oblivion::SoulGemType::Black), 5);

    // --- a weapon enchantment's charge cost is the unadjusted sum of effect base costs ---
    Oblivion::Enchantment fireStrike;
    fireStrike.mName = "Fire Damage";
    fireStrike.mType = Oblivion::EnchantmentType::CastWhenStrikes;
    fireStrike.mEffects = { makeEffect(ESM4::EFI_FIDG, Oblivion::SpellRange::Touch, 3, 3, 1) };
    const std::int32_t chargeCost = Oblivion::weaponEnchantmentChargeCost(fireStrike);
    EXPECT_EQ(chargeCost, 3); // floor(0.75 * 3^1.28)
    EXPECT_EQ(Oblivion::weaponEnchantmentUses(16, chargeCost), 5);
    EXPECT_EQ(Oblivion::weaponEnchantmentUses(16, 0), 0);
    EXPECT_FLOAT_EQ(Oblivion::enchantedWeaponValueBonus(chargeCost, 16), 0.4f * 19.f);

    // --- alchemy: a Master Alchemy brew with a full apparatus ---
    const Oblivion::AlchemyIngredient apple{ { ESM4::EFI_REHE, ESM4::EFI_DGHE }, 0.5f };
    const Oblivion::AlchemyIngredient pear{ { ESM4::EFI_REHE, ESM4::EFI_DGHE }, 0.3f };

    Oblivion::AlchemyApparatus noviceApparatus; // novice mortar only
    Oblivion::AlchemyApparatus masterApparatus; // master set
    masterApparatus.mMortar = Oblivion::ApparatusQuality::Master;
    masterApparatus.mHasRetort = 1;
    masterApparatus.mRetort = Oblivion::ApparatusQuality::Master;
    masterApparatus.mHasCalcinator = 1;
    masterApparatus.mCalcinator = Oblivion::ApparatusQuality::Master;
    masterApparatus.mHasAlembic = 1;
    masterApparatus.mAlembic = Oblivion::ApparatusQuality::Master;

    // The positive effect shared by both ingredients is stronger from a master apparatus;
    // compare it across brews (the Alembic reduces the negative effect, so compare only
    // the Restore Health entry).
    const std::vector<Oblivion::PotionEffect> simplePotion =
        Oblivion::brewPotion({ apple, pear }, 50, 50, noviceApparatus);
    const std::vector<Oblivion::PotionEffect> masterPotion =
        Oblivion::brewPotion({ apple, pear }, 100, 100, masterApparatus);
    ASSERT_EQ(simplePotion.size(), 2u);
    ASSERT_EQ(masterPotion.size(), 2u);
    const auto findEffect = [](const std::vector<Oblivion::PotionEffect>& effects, std::uint32_t id) {
        for (const Oblivion::PotionEffect& effect : effects)
            if (effect.mEffectId == id)
                return &effect;
        return static_cast<const Oblivion::PotionEffect*>(nullptr);
    };
    const Oblivion::PotionEffect* simpleRestore = findEffect(simplePotion, ESM4::EFI_REHE);
    const Oblivion::PotionEffect* masterRestore = findEffect(masterPotion, ESM4::EFI_REHE);
    ASSERT_NE(simpleRestore, nullptr);
    ASSERT_NE(masterRestore, nullptr);
    EXPECT_GE(masterRestore->mMagnitude, simpleRestore->mMagnitude);
    EXPECT_GE(masterRestore->mDuration, simpleRestore->mDuration);

    // Alchemy's effective value and the sale price of the brew.
    EXPECT_EQ(Oblivion::effectiveAlchemyValue(50, 50), 50);
    EXPECT_EQ(Oblivion::effectiveAlchemyValue(50, 100), 70); // +0.4 per Luck point above 50
    EXPECT_EQ(Oblivion::effectiveAlchemyValue(50, 0), 30);
    EXPECT_EQ(Oblivion::potionSalePrice(50, 50, Oblivion::ApparatusQuality::Novice), 23);
    EXPECT_GT(Oblivion::potionSalePrice(100, 100, Oblivion::ApparatusQuality::Master),
        Oblivion::potionSalePrice(50, 50, Oblivion::ApparatusQuality::Novice));
    EXPECT_FLOAT_EQ(Oblivion::potionWeight({ apple, pear }), 0.4f);

    // Alchemy identifies up to four effects at Expert rank; a Master brews from one ingredient.
    EXPECT_EQ(Oblivion::alchemyIdentifiedEffects(0), 1);
    EXPECT_EQ(Oblivion::alchemyIdentifiedEffects(25), 2);
    EXPECT_EQ(Oblivion::alchemyIdentifiedEffects(50), 3);
    EXPECT_EQ(Oblivion::alchemyIdentifiedEffects(80), 4);
    EXPECT_TRUE(Oblivion::alchemyCanBrewSingleIngredient(100));
    EXPECT_FALSE(Oblivion::alchemyCanBrewSingleIngredient(99));
}

// ---------- stage 5: leveling, training and the snapshot machinery ----------

TEST(OblivionComplete, levelingFlowTrainingAndSnapshotSurviveAFullLevel)
{
    const Oblivion::ClassDefinition cls = makeCombatClass();

    Oblivion::ActorStats stats(50, 20, 80);
    stats.setAttribute(Oblivion::Attribute::Strength, 40);
    stats.setAttribute(Oblivion::Attribute::Endurance, 12);
    stats.setAttribute(Oblivion::Attribute::Luck, 70);
    stats.setClass(cls);
    stats.initializeFromClass();

    // Practice ten times, then take the level up.
    for (int i = 0; i < 10; ++i)
        EXPECT_FALSE(stats.raiseSkill(Oblivion::Skill::Blade, 1));
    ASSERT_TRUE(stats.isLevelUpReady());
    EXPECT_EQ(stats.getSkill(Oblivion::Skill::Blade), 40); // 30 + 10

    // Training a major skill counts towards the same bookkeeping.
    ASSERT_TRUE(stats.trainSkill(Oblivion::Skill::Block));
    EXPECT_EQ(stats.getTrainingsUsedThisLevel(), 1);
    EXPECT_EQ(stats.getMajorSkillIncreases(), 11);

    const Oblivion::AttributesSnapshot snapshot = stats.snapshot();

    // Break the state, then restore it as a save load would.
    stats.setSkill(Oblivion::Skill::Blade, 5);
    stats.setAttribute(Oblivion::Attribute::Strength, 1);
    stats.setAttribute(Oblivion::Attribute::Luck, 1);
    stats.restoreFromSnapshot(snapshot);

    EXPECT_EQ(stats.getSkill(Oblivion::Skill::Blade), 40);
    EXPECT_EQ(stats.getAttribute(Oblivion::Attribute::Strength), 40);
    EXPECT_EQ(stats.getAttribute(Oblivion::Attribute::Luck), 70);
    EXPECT_EQ(stats.getMajorSkillIncreases(), 11);
    EXPECT_EQ(stats.getTrainingsUsedThisLevel(), 1);
    EXPECT_EQ(stats.getLevel(), 1);
    ASSERT_TRUE(stats.isLevelUpReady());

    // Level-up bookkeeping is deducted, not just validated: 11 - 10 (the training point
    // rolls over towards the next level); per-attribute counters and trainings reset.
    const std::array<Oblivion::Attribute, 3> chosen = { Oblivion::Attribute::Strength,
        Oblivion::Attribute::Endurance, Oblivion::Attribute::Agility };
    ASSERT_TRUE(stats.levelUp(chosen));
    EXPECT_EQ(stats.getLevel(), 2);
    EXPECT_EQ(stats.getMajorSkillIncreases(), 1);
    EXPECT_EQ(stats.skillIncreasesForAttribute(Oblivion::Attribute::Strength), 0);
    EXPECT_EQ(stats.getTrainingsUsedThisLevel(), 0);
    EXPECT_FALSE(stats.levelUp(chosen)); // not ready anymore
}

// ---------- stage 5: AI behaviour across a fight ----------

TEST(OblivionComplete, aiNpcBehaviourProgressesFromPackageToFightToYield)
{
    // A calm NPC of the loaded world follows its package; a hostile one fights; a broken
    // one flees; an enraged one fights to the death or yields at the brink.
    Oblivion::AiContext wander;
    wander.mData.mConfidence = 50;
    wander.mData.mDisposition = 80; // friendly
    wander.mCurrentPackage = Oblivion::PackageType::Wander;
    const Oblivion::AiDecision wanderDecision = Oblivion::evaluateAiBehavior(wander);
    EXPECT_EQ(wanderDecision.mState, Oblivion::BehaviorState::Wander);
    EXPECT_FALSE(wanderDecision.mShouldInitiateCombat);

    // The player insults the NPC: aggression 80 vs a hated disposition starts a fight.
    Oblivion::AiContext hostile;
    hostile.mData.mAggression = 80;
    hostile.mData.mConfidence = 50;
    hostile.mData.mResponsibility = 50;
    hostile.mData.mDisposition = 10;
    hostile.mTargetVisible = true;
    hostile.mTargetIsFoe = true;
    hostile.mHealthFraction = 1.f;
    const Oblivion::AiDecision fightDecision = Oblivion::evaluateAiBehavior(hostile);
    EXPECT_EQ(fightDecision.mState, Oblivion::BehaviorState::Fight);
    EXPECT_TRUE(fightDecision.mShouldInitiateCombat);
    EXPECT_EQ(Oblivion::chooseCombatBehavior(hostile), Oblivion::BehaviorState::Fight);

    // The NPC wears down: below (100 - confidence)% health it flees.
    hostile.mHealthFraction = 0.3f;
    const Oblivion::AiDecision fleeDecision = Oblivion::evaluateAiBehavior(hostile);
    EXPECT_EQ(fleeDecision.mState, Oblivion::BehaviorState::Flee);
    EXPECT_EQ(Oblivion::chooseCombatBehavior(hostile), Oblivion::BehaviorState::Flee);

    // A 100-confidence veteran never flees, but yields at the brink of death.
    hostile.mData.mConfidence = 100;
    hostile.mHealthFraction = 0.05f;
    hostile.mData.mDisposition = 50;
    const Oblivion::AiDecision yieldDecision = Oblivion::evaluateAiBehavior(hostile);
    EXPECT_EQ(yieldDecision.mState, Oblivion::BehaviorState::Yield);

    // Disposition shifts with the player's fame and infamy, and NPCs speak up about crimes.
    EXPECT_EQ(Oblivion::dispositionModifierFromFame(67), 20); // capped at +20
    EXPECT_EQ(Oblivion::dispositionModifierFromInfamy(32, 100), -16);
    EXPECT_EQ(Oblivion::dispositionTone(10), Oblivion::DispositionTone::Hostile);
    EXPECT_EQ(Oblivion::dispositionTone(50), Oblivion::DispositionTone::Neutral);
    EXPECT_EQ(Oblivion::dispositionTone(80), Oblivion::DispositionTone::Friendly);

    Oblivion::AiContext crime;
    crime.mCrimeWitnessed = true;
    crime.mData.mResponsibility = 50;
    EXPECT_TRUE(Oblivion::evaluateAiBehavior(crime).mReportsCrime);
    crime.mData.mResponsibility = 10;
    EXPECT_FALSE(Oblivion::evaluateAiBehavior(crime).mReportsCrime);
}

TEST(OblivionComplete, gameModeSwitcherDrivesWholeEngineModeSession)
{
    RecordingGameModeSwitcher switcher;
    Oblivion::MechanicsSystem mechanics(&switcher);

    EXPECT_EQ(mechanics.getGameMode(), Oblivion::GameMode::Game);
    EXPECT_FALSE(mechanics.isInGameMode());

    // Walk every Oblivion gameplay mode the engine exposes, in the order a session uses them.
    const std::array<Oblivion::GameMode, 14> sessionModes = { Oblivion::GameMode::Inventory,
        Oblivion::GameMode::Alchemy, Oblivion::GameMode::SpellMaking, Oblivion::GameMode::Enchanting,
        Oblivion::GameMode::Repair, Oblivion::GameMode::Training, Oblivion::GameMode::Barter,
        Oblivion::GameMode::SpellBuying, Oblivion::GameMode::Container, Oblivion::GameMode::Companion,
        Oblivion::GameMode::Recharge, Oblivion::GameMode::MerchantRepair, Oblivion::GameMode::LevelUp,
        Oblivion::GameMode::Rest };

    for (const Oblivion::GameMode mode : sessionModes)
    {
        ASSERT_TRUE(mechanics.requestGameMode(mode)) << Oblivion::gameModeName(mode);
        EXPECT_EQ(mechanics.getGameMode(), mode);
        EXPECT_TRUE(mechanics.isInGameMode());
        mechanics.leaveGameMode();
        EXPECT_EQ(mechanics.getGameMode(), Oblivion::GameMode::Game);
        EXPECT_FALSE(mechanics.isInGameMode());
    }

    // The switcher observed exactly one request per mode, followed by Game on leave;
    // every name is unique.
    ASSERT_EQ(switcher.mRequestedModes.size(), 2 * sessionModes.size());
    for (std::size_t i = 0; i < sessionModes.size(); ++i)
    {
        EXPECT_EQ(switcher.mRequestedModes[2 * i], sessionModes[i]);
        EXPECT_EQ(switcher.mRequestedModes[2 * i + 1], Oblivion::GameMode::Game); // leaveGameMode()
    }

    std::vector<std::string_view> names;
    for (const Oblivion::GameMode mode : sessionModes)
        names.push_back(Oblivion::gameModeName(mode));
    names.push_back(Oblivion::gameModeName(Oblivion::GameMode::Game));
    std::sort(names.begin(), names.end());
    EXPECT_EQ(std::adjacent_find(names.begin(), names.end()), names.end()); // all unique

    // A rejecting engine vetoes a mode change (e.g. the inventory UI refuses to open).
    switcher.mReject = true;
    EXPECT_FALSE(mechanics.requestGameMode(Oblivion::GameMode::Alchemy));
    EXPECT_EQ(mechanics.getGameMode(), Oblivion::GameMode::Game);
    switcher.mReject = false;

    // Headless mechanics (no switcher attached) accepts every transition.
    Oblivion::MechanicsSystem headless;
    EXPECT_TRUE(headless.requestGameMode(Oblivion::GameMode::Enchanting));
    EXPECT_EQ(headless.getGameMode(), Oblivion::GameMode::Enchanting);
    EXPECT_TRUE(headless.isInGameMode());
}

// ---------- the sentinel: verbose session log is safe ----------

TEST(OblivionComplete, verboseSessionLogsTheStartupReport)
{
    const std::filesystem::path dir = writeStandardFixture("verbose-log");

    const std::vector<std::string> contentFiles = { "CompleteMaster.esm", "CompleteMod.esp" };
    const Files::Collections collections({ dir });

    Oblivion::Configuration verbose;
    verbose.mVerbose = true;
    const Oblivion::ContentReport report = Oblivion::checkContentFiles(contentFiles, collections, verbose);
    EXPECT_EQ(report.mOblivionContentCount, 2u);
    expectNoErrors(report);
    EXPECT_TRUE(hasMessageContaining(report, "CompleteMaster.esm")) << issueSummary(report);

    // Logging the report (one line per issue and a summary) must not crash.
    Oblivion::logContentReport(report);
}
