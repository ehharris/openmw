// Oblivion (TES4) content loading tests.
//
// These tests exercise the complete Oblivion content loading sequence as it is performed
// when the engine starts up with Oblivion content configured:
//
//   1. the configured content files (and groundcover files) are checked for Oblivion
//      (TES4-family) plugins/masters by Oblivion::checkContentFiles (components/oblivion/config.hpp),
//      which validates each file and its master dependencies (availability and load order),
//   2. each file is located through Files::Collections (the configured data directories),
//   3. each Oblivion plugin/master is detected, validated and loaded in load order by
//      Oblivion::ContentLoader (components/oblivion/contentloader.hpp), exactly like the
//      engine's content loading pipeline (apps/openmw/mwworld/esmloader.cpp) does before
//      handing the file over to the streaming reader,
//   4. the same files are read by the ESM4 reader that the engine uses to stream records,
//      to verify the Oblivion-specific view and the generic TES4-family view agree.
//
// The tests build synthetic Oblivion plugin files in a temporary directory, so they are
// self-contained and do not require any game data. They live at the path requested by the
// task brief; they are built as part of the components-tests target (see
// apps/components_tests/CMakeLists.txt), which is where all component and integration
// tests in this repository are built and run.

#include <components/oblivion/contentloader.hpp>
#include <components/oblivion/config.hpp>

#include <components/esm/common.hpp>
#include <components/esm/format.hpp>
#include <components/esm/fourcc.hpp>
#include <components/esm/gameformat.hpp>
#include <components/esm4/reader.hpp>
#include <components/files/collections.hpp>
#include <components/files/openfile.hpp>
#include <components/files/oblivion/esm.hpp>
#include <components/testing/util.hpp>

#include <gtest/gtest.h>

#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <string_view>
#include <vector>

namespace
{
    // Little-endian byte builder used to construct synthetic plugin files.
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

    std::vector<char> makeU32SubRecord(std::uint32_t type, std::uint32_t value)
    {
        BytesBuilder builder;
        builder.u32(value);
        return makeSubRecord(type, builder.bytes);
    }

    std::vector<char> makeFormIdSubRecord(std::uint32_t type, std::uint32_t formId)
    {
        return makeU32SubRecord(type, formId);
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
        return TestingOpenMW::outputDirPath(std::filesystem::path("oblivion-loading") / std::string(name));
    }

    std::filesystem::path writeInDir(const std::filesystem::path& dir, const std::string& name,
        const std::vector<char>& bytes)
    {
        const std::filesystem::path path = dir / name;
        writeFile(path, bytes);
        return path;
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

    // A small but representative Oblivion world: a WRLD record plus an interior cell,
    // wrapped in the group hierarchy the game uses.
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

    bool hasMessageContaining(const Oblivion::ContentReport& report, std::string_view needle)
    {
        for (const Oblivion::ConfigIssue& issue : report.mIssues)
            if (issue.mMessage.find(needle) != std::string::npos)
                return true;
        return false;
    }

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

    void expectNoIssues(const Oblivion::ContentReport& report)
    {
        EXPECT_EQ(report.mIssues.size(), 0u) << issueSummary(report);
        EXPECT_FALSE(report.hasErrors());
        EXPECT_FALSE(report.hasWarnings());
    }

    void expectNoErrors(const Oblivion::ContentReport& report)
    {
        EXPECT_FALSE(report.hasErrors()) << issueSummary(report);
    }
}

// The full Oblivion loading sequence, performed in load order: startup content check,
// per-file format detection and validation, the Oblivion content loader and the ESM4
// reader the engine hands the file to.
TEST(OblivionLoading, fullOblivionLoadingSequenceLoadsMasterAndPlugin)
{
    const std::filesystem::path dir = makeDataDir("full-sequence");

    PluginOptions masterOptions;
    masterOptions.hedrVersion = ESM::VER_080; // pre-1.1 plugins are valid Oblivion content
    masterOptions.master = true;
    masterOptions.author = "Test Author";
    masterOptions.description = "Synthetic Oblivion master";
    writeInDir(dir, "SequenceMaster.esm", buildPlugin(masterOptions, buildWorldCellContent()));

    PluginOptions pluginOptions;
    pluginOptions.hedrVersion = ESM::VER_100; // vanilla Oblivion header version
    pluginOptions.master = false;
    pluginOptions.masters = { { "SequenceMaster.esm", 0x0000abcd } };
    writeInDir(dir, "SequenceMod.esp", buildPlugin(pluginOptions));

    const std::vector<std::string> contentFiles = { "SequenceMaster.esm", "SequenceMod.esp" };

    // Step 1: the startup content check, as performed by the engine before loading.
    const Files::Collections collections({ dir });
    const Oblivion::Configuration config; // validation on, non-strict, TES3 content tolerated
    const Oblivion::ContentReport report = Oblivion::checkContentFiles(contentFiles, collections, config);
    EXPECT_EQ(report.mOblivionContentCount, 2u);
    expectNoIssues(report);

    // Step 2: for every configured file, detect the on-disk format and confirm it is Oblivion content.
    for (const std::string& file : contentFiles)
    {
        const std::filesystem::path path = dir / file;

        auto stream = Files::openBinaryInputFileStream(path);
        ASSERT_TRUE(stream);
        ASSERT_TRUE(stream->is_open());
        EXPECT_EQ(ESM::Format::Tes4, ESM::readFormat(*stream));

        ASSERT_TRUE(Oblivion::ContentLoader::isOblivionPlugin(path));
        EXPECT_NO_THROW(Oblivion::ContentLoader::validate(path));
    }

    // Step 3: load each file in load order, like the engine's content loading pipeline.
    const Oblivion::ContentLoader loader;

    const Oblivion::ContentFile master = loader.load(dir / "SequenceMaster.esm");
    EXPECT_TRUE(master.mMaster); // determined by the Rec_ESM flag, not the file extension
    EXPECT_EQ(master.mHedrVersion, ESM::VER_080);
    EXPECT_EQ(master.mAuthor, "Test Author");
    EXPECT_EQ(master.mDescription, "Synthetic Oblivion master");
    ASSERT_TRUE(master.mMasters.empty());
    EXPECT_EQ(master.getRecordCount(Files::Oblivion::REC_WRLD), 1u);
    EXPECT_EQ(master.getRecordCount(Files::Oblivion::REC_CELL), 2u);
    EXPECT_EQ(master.getRecordCount(Files::Oblivion::REC_REFR), 1u);
    EXPECT_EQ(master.getTotalRecordCount(), 4u);
    ASSERT_EQ(master.mWorlds.size(), 1u);
    EXPECT_EQ(master.mWorlds.front().mFormId, 0x00000010u);
    EXPECT_EQ(master.mWorlds.front().mCells, std::vector<std::uint32_t>{ 0x00000020u });
    ASSERT_EQ(master.mCells.size(), 2u);
    EXPECT_TRUE(master.getCells(0x00000010u).size() == 1u);

    const Oblivion::ContentFile mod = loader.load(dir / "SequenceMod.esp");
    EXPECT_FALSE(mod.mMaster);
    EXPECT_EQ(mod.mHedrVersion, ESM::VER_100);
    ASSERT_EQ(mod.mMasters.size(), 1u);
    EXPECT_EQ(mod.mMasters[0].mName, "SequenceMaster.esm");
    EXPECT_EQ(mod.mMasters[0].mSize, 0x0000abcdu);
    EXPECT_TRUE(Oblivion::ContentLoader::findMissingMasters(mod, collections).empty());
    EXPECT_TRUE(Oblivion::ContentLoader::findMissingMasters(mod, contentFiles).empty());
    EXPECT_TRUE(Oblivion::ContentLoader::findMissingMasters(master, collections).empty());

    // Step 4: the streaming reader the engine hands the record data to reads the same files.
    {
        const std::filesystem::path masterPath = dir / "SequenceMaster.esm";
        ESM4::Reader esm4Master(
            Files::openBinaryInputFileStream(masterPath), masterPath, /*vfs=*/nullptr, /*encoder=*/nullptr);
        EXPECT_EQ(esm4Master.gameFormat(), ESM::GameFormat::TES4);
        EXPECT_EQ(esm4Master.esmVersion(), ESM::VER_080);
        EXPECT_TRUE(esm4Master.getGameFiles().empty());
        EXPECT_EQ(esm4Master.getRecordCount(), 0);
        EXPECT_EQ(esm4Master.getAuthor(), "Test Author");
        EXPECT_EQ(esm4Master.getDesc(), "Synthetic Oblivion master");
    }
    {
        const std::filesystem::path modPath = dir / "SequenceMod.esp";
        ESM4::Reader esm4Mod(
            Files::openBinaryInputFileStream(modPath), modPath, /*vfs=*/nullptr, /*encoder=*/nullptr);
        EXPECT_EQ(esm4Mod.gameFormat(), ESM::GameFormat::TES4);
        ASSERT_EQ(esm4Mod.getGameFiles().size(), 1u);
        EXPECT_EQ(esm4Mod.getGameFiles().front().name, "SequenceMaster.esm");
    }
}

TEST(OblivionLoading, wrongLoadOrderIsAWarningByDefaultAndAnErrorInStrictMode)
{
    const std::filesystem::path dir = makeDataDir("load-order");

    PluginOptions masterOptions;
    masterOptions.hedrVersion = ESM::VER_100;
    masterOptions.master = true;
    writeInDir(dir, "OrderedMaster.esm", buildPlugin(masterOptions));

    PluginOptions pluginOptions;
    pluginOptions.hedrVersion = ESM::VER_100;
    pluginOptions.master = false;
    pluginOptions.masters = { { "OrderedMaster.esm", 1 } };
    writeInDir(dir, "OrderedPlugin.esp", buildPlugin(pluginOptions));

    // The plugin is listed before the master it depends on.
    const std::vector<std::string> contentFiles = { "OrderedPlugin.esp", "OrderedMaster.esm" };
    const Files::Collections collections({ dir });

    const Oblivion::Configuration config;
    const Oblivion::ContentReport report = Oblivion::checkContentFiles(contentFiles, collections, config);
    EXPECT_EQ(report.mOblivionContentCount, 2u);
    EXPECT_FALSE(report.hasErrors()) << issueSummary(report);
    EXPECT_TRUE(report.hasWarnings());
    EXPECT_TRUE(hasMessageContaining(report, "is listed after")) << issueSummary(report);

    Oblivion::Configuration strict;
    strict.mStrictLoadOrder = true;
    const Oblivion::ContentReport strictReport = Oblivion::checkContentFiles(contentFiles, collections, strict);
    EXPECT_TRUE(strictReport.hasErrors()) << issueSummary(strictReport);
}

TEST(OblivionLoading, missingMasterIsAWarningByDefaultAndAnErrorInStrictMode)
{
    const std::filesystem::path dir = makeDataDir("missing-master");

    PluginOptions pluginOptions;
    pluginOptions.hedrVersion = ESM::VER_100;
    pluginOptions.master = false;
    pluginOptions.masters = { { "Absent.esm", 1 } };
    writeInDir(dir, "NeedsAbsent.esp", buildPlugin(pluginOptions));

    const std::vector<std::string> contentFiles = { "NeedsAbsent.esp" };
    const Files::Collections collections({ dir });

    const Oblivion::Configuration config;
    const Oblivion::ContentReport report = Oblivion::checkContentFiles(contentFiles, collections, config);
    EXPECT_EQ(report.mOblivionContentCount, 1u);
    EXPECT_FALSE(report.hasErrors()) << issueSummary(report);
    EXPECT_TRUE(report.hasWarnings());
    EXPECT_TRUE(hasMessageContaining(report, "requires master file 'Absent.esm'")) << issueSummary(report);

    Oblivion::Configuration strict;
    strict.mStrictLoadOrder = true;
    const Oblivion::ContentReport strictReport = Oblivion::checkContentFiles(contentFiles, collections, strict);
    EXPECT_TRUE(strictReport.hasErrors()) << issueSummary(strictReport);
}

TEST(OblivionLoading, masterPresentInDataDirButNotInContentListIsReported)
{
    const std::filesystem::path dir = makeDataDir("unfetched-master");

    PluginOptions masterOptions;
    masterOptions.hedrVersion = ESM::VER_100;
    masterOptions.master = true;
    writeInDir(dir, "UnfetchedMaster.esm", buildPlugin(masterOptions));

    PluginOptions pluginOptions;
    pluginOptions.hedrVersion = ESM::VER_100;
    pluginOptions.master = false;
    pluginOptions.masters = { { "UnfetchedMaster.esm", 1 } };
    writeInDir(dir, "NeedsUnfetched.esp", buildPlugin(pluginOptions));

    // The master is in the data directory but not in the configured content list;
    // the plugin will fail to resolve its dependency at load time.
    const std::vector<std::string> contentFiles = { "NeedsUnfetched.esp" };
    const Files::Collections collections({ dir });

    const Oblivion::Configuration config;
    const Oblivion::ContentReport report = Oblivion::checkContentFiles(contentFiles, collections, config);
    EXPECT_EQ(report.mOblivionContentCount, 1u);
    EXPECT_FALSE(report.hasErrors()) << issueSummary(report);
    EXPECT_TRUE(report.hasWarnings());
    EXPECT_TRUE(hasMessageContaining(report, "must also be added")) << issueSummary(report);

    // The loader itself reports the master as missing against the configured content list...
    const Oblivion::ContentFile plugin = Oblivion::ContentLoader().load(dir / "NeedsUnfetched.esp");
    const std::vector<std::string> missingFromContent = Oblivion::ContentLoader::findMissingMasters(plugin, contentFiles);
    ASSERT_EQ(missingFromContent.size(), 1u);
    EXPECT_EQ(missingFromContent.front(), "UnfetchedMaster.esm");

    // ...but it is available through the data directories.
    EXPECT_TRUE(Oblivion::ContentLoader::findMissingMasters(plugin, collections).empty());
}

TEST(OblivionLoading, morrowindContentIsReportedAndSkippedByTheOblivionCheck)
{
    const std::filesystem::path dir = makeDataDir("mixed-content");

    writeInDir(dir, "Morrowind.esm", buildTes3File());

    PluginOptions masterOptions;
    masterOptions.hedrVersion = ESM::VER_100;
    masterOptions.master = true;
    writeInDir(dir, "RealOblivionMaster.esm", buildPlugin(masterOptions));

    const std::vector<std::string> contentFiles = { "Morrowind.esm", "RealOblivionMaster.esm" };
    const Files::Collections collections({ dir });

    const Oblivion::Configuration config;
    const Oblivion::ContentReport report = Oblivion::checkContentFiles(contentFiles, collections, config);
    EXPECT_EQ(report.mOblivionContentCount, 1u);
    expectNoErrors(report);
    EXPECT_TRUE(hasMessageContaining(report, "Morrowind")) << issueSummary(report);

    // Format detection agrees on what is and isn't Oblivion content.
    auto tes3Stream = Files::openBinaryInputFileStream(dir / "Morrowind.esm");
    ASSERT_TRUE(tes3Stream);
    EXPECT_EQ(ESM::Format::Tes3, ESM::readFormat(*tes3Stream));
    EXPECT_EQ(ESM::getGameFormat(ESM::VER_130, std::nullopt), ESM::GameFormat::TES3);
    EXPECT_EQ(ESM::getGameFormat(ESM::VER_080, std::nullopt), ESM::GameFormat::TES4);
}

TEST(OblivionLoading, tes4PluginWithNonOblivionHedrIsRejectedOnlyWhenForced)
{
    const std::filesystem::path dir = makeDataDir("non-oblivion-tes4");

    PluginOptions fo3Options;
    fo3Options.hedrVersion = ESM::VER_094; // Fallout 3: TES4 signature, but not an Oblivion HEDR version
    fo3Options.master = true;
    writeInDir(dir, "Fallout3.esm", buildPlugin(fo3Options));

    const std::vector<std::string> contentFiles = { "Fallout3.esm" };
    const Files::Collections collections({ dir });

    // Without forcing Oblivion mode, the file is a warning and is not loaded as Oblivion content.
    const Oblivion::Configuration config;
    const Oblivion::ContentReport report = Oblivion::checkContentFiles(contentFiles, collections, config);
    EXPECT_EQ(report.mOblivionContentCount, 0u);
    expectNoErrors(report);
    EXPECT_TRUE(report.hasWarnings());
    EXPECT_TRUE(hasMessageContaining(report, "not recognised as Oblivion")) << issueSummary(report);

    // Forced Oblivion mode makes configuring a non-Oblivion plugin fatal.
    Oblivion::Configuration forced;
    forced.mForced = true;
    forced.mValidate = true;
    const Oblivion::ContentReport forcedReport = Oblivion::checkContentFiles(contentFiles, collections, forced);
    EXPECT_TRUE(forcedReport.hasErrors()) << issueSummary(forcedReport);

    // The loader itself refuses the file.
    EXPECT_FALSE(Oblivion::ContentLoader::isOblivionPlugin(dir / "Fallout3.esm"));
    EXPECT_THROW(Oblivion::ContentLoader::validate(dir / "Fallout3.esm"), std::runtime_error);
    EXPECT_THROW(Oblivion::ContentLoader().load(dir / "Fallout3.esm"), std::runtime_error);
}

// A plugin whose header parses (so the startup check passes it through) but whose body is
// corrupt fails when the engine actually loads it. The content loading tests must surface
// this as an exception from load(), not as silent data corruption.
TEST(OblivionLoading, corruptPluginPassesValidationButFailsOnLoad)
{
    const std::filesystem::path dir = makeDataDir("corrupt-body");

    PluginOptions options;
    options.hedrVersion = ESM::VER_100;
    options.master = true;

    // A GRUP whose declared group size is smaller than its own 20-byte header.
    BytesBuilder corruptGroup;
    corruptGroup.u32(ESM::fourCC("GRUP"));
    corruptGroup.u32(10); // groupSize < sizeof(GroupHeader)
    corruptGroup.u32(0);
    corruptGroup.i32(Files::Oblivion::Grp_RecordType);
    corruptGroup.u32(0);
    corruptGroup.u32(0);
    writeInDir(dir, "Corrupt.esm", buildPlugin(options, corruptGroup.bytes));

    const std::vector<std::string> contentFiles = { "Corrupt.esm" };
    const Files::Collections collections({ dir });

    // The header is fine, so the startup check passes the file through for loading.
    const Oblivion::Configuration config;
    const Oblivion::ContentReport report = Oblivion::checkContentFiles(contentFiles, collections, config);
    EXPECT_EQ(report.mOblivionContentCount, 1u);
    expectNoErrors(report);

    // Loading the body fails with an exception.
    EXPECT_THROW(Oblivion::ContentLoader().load(dir / "Corrupt.esm"), std::runtime_error);
}

TEST(OblivionLoading, emptyContentListProducesEmptyReport)
{
    const std::filesystem::path dir = makeDataDir("empty-content");
    const Files::Collections collections({ dir });

    const Oblivion::Configuration config;
    const Oblivion::ContentReport report = Oblivion::checkContentFiles({}, collections, config);
    EXPECT_EQ(report.mOblivionContentCount, 0u);
    expectNoIssues(report);
}

TEST(OblivionLoading, contentFileMissingFromDiskIsAnError)
{
    const std::filesystem::path dir = makeDataDir("missing-file");
    const Files::Collections collections({ dir });

    const std::vector<std::string> contentFiles = { "DoesNotExist.esm" };

    const Oblivion::Configuration config;
    const Oblivion::ContentReport report = Oblivion::checkContentFiles(contentFiles, collections, config);
    EXPECT_EQ(report.mOblivionContentCount, 0u);
    EXPECT_TRUE(report.hasErrors());
    EXPECT_TRUE(hasMessageContaining(report, "does not exist")) << issueSummary(report);
}

// A master/plugin chain of three files: every file must load in the order given,
// and every dependency must resolve against the files earlier in the load order.
TEST(OblivionLoading, multiplePluginLoadOrderIsCheckedAcrossFiles)
{
    const std::filesystem::path dir = makeDataDir("multi-load-order");

    PluginOptions oblivionMaster;
    oblivionMaster.hedrVersion = ESM::VER_100;
    oblivionMaster.master = true;
    oblivionMaster.author = "Bethesda";
    writeInDir(dir, "Oblivion.esm", buildPlugin(oblivionMaster));

    PluginOptions updateMaster;
    updateMaster.hedrVersion = ESM::VER_100;
    updateMaster.master = true;
    updateMaster.author = "Bethesda";
    updateMaster.masters = { { "Oblivion.esm", 123 } };
    writeInDir(dir, "Update.esm", buildPlugin(updateMaster));

    PluginOptions mod;
    mod.hedrVersion = ESM::VER_080;
    mod.master = false;
    mod.masters = { { "Oblivion.esm", 111 }, { "Update.esm", 222 } };
    writeInDir(dir, "Mod.esp", buildPlugin(mod));

    const Files::Collections collections({ dir });
    const Oblivion::Configuration config;

    const std::vector<std::string> contentFiles = { "Oblivion.esm", "Update.esm", "Mod.esp" };
    const Oblivion::ContentReport report = Oblivion::checkContentFiles(contentFiles, collections, config);
    EXPECT_EQ(report.mOblivionContentCount, 3u);
    expectNoIssues(report);

    const Oblivion::ContentLoader loader;
    const Oblivion::ContentFile modContent = loader.load(dir / "Mod.esp");
    ASSERT_EQ(modContent.mMasters.size(), 2u);
    EXPECT_EQ(modContent.mMasters[0].mName, "Oblivion.esm");
    EXPECT_EQ(modContent.mMasters[0].mSize, 111u);
    EXPECT_EQ(modContent.mMasters[1].mName, "Update.esm");
    EXPECT_EQ(modContent.mMasters[1].mSize, 222u);
    EXPECT_TRUE(Oblivion::ContentLoader::findMissingMasters(modContent, contentFiles).empty());

    // Update.esm depends on Oblivion.esm, so listing them the other way round breaks both dependants.
    const std::vector<std::string> reversed = { "Update.esm", "Oblivion.esm", "Mod.esp" };
    const Oblivion::ContentReport reversedReport = Oblivion::checkContentFiles(reversed, collections, config);
    EXPECT_EQ(reversedReport.mOblivionContentCount, 3u);
    EXPECT_FALSE(reversedReport.hasErrors()) << issueSummary(reversedReport);
    EXPECT_TRUE(reversedReport.hasWarnings());
    EXPECT_TRUE(hasMessageContaining(reversedReport, "is listed after")) << issueSummary(reversedReport);
}

TEST(OblivionLoading, verboseModeEmitsInfoIssuesPerOblivionFile)
{
    const std::filesystem::path dir = makeDataDir("verbose");

    PluginOptions options;
    options.hedrVersion = ESM::VER_100;
    options.master = true;
    writeInDir(dir, "VerboseMaster.esm", buildPlugin(options));

    const std::vector<std::string> contentFiles = { "VerboseMaster.esm" };
    const Files::Collections collections({ dir });

    Oblivion::Configuration verbose;
    verbose.mVerbose = true;
    const Oblivion::ContentReport verboseReport = Oblivion::checkContentFiles(contentFiles, collections, verbose);
    EXPECT_EQ(verboseReport.mOblivionContentCount, 1u);
    expectNoErrors(verboseReport);
    EXPECT_TRUE(hasMessageContaining(verboseReport, "VerboseMaster.esm")) << issueSummary(verboseReport);
    Oblivion::logContentReport(verboseReport); // must not crash

    // Without verbose mode the same file produces no issues at all.
    const Oblivion::Configuration quiet;
    const Oblivion::ContentReport quietReport = Oblivion::checkContentFiles(contentFiles, collections, quiet);
    EXPECT_EQ(quietReport.mOblivionContentCount, 1u);
    expectNoIssues(quietReport);
}

TEST(OblivionLoading, oblivionSupportDisabledMakesConfiguredPluginsFatal)
{
    const std::filesystem::path dir = makeDataDir("disabled");

    PluginOptions options;
    options.hedrVersion = ESM::VER_100;
    options.master = true;
    writeInDir(dir, "DisabledMaster.esm", buildPlugin(options));

    const std::vector<std::string> contentFiles = { "DisabledMaster.esm" };
    const Files::Collections collections({ dir });

    // [Oblivion] enable = false in settings.cfg turns configured Oblivion content into an error.
    Oblivion::Configuration disabled;
    disabled.mEnable = false;
    const Oblivion::ContentReport report = Oblivion::checkContentFiles(contentFiles, collections, disabled);
    EXPECT_EQ(report.mOblivionContentCount, 0u);
    EXPECT_TRUE(report.hasErrors()) << issueSummary(report);
    EXPECT_TRUE(hasMessageContaining(report, "disabled")) << issueSummary(report);
}

// Validation disabled: Oblivion content is counted but not checked any further.
TEST(OblivionLoading, validationDisabledSkipsHeaderValidationButKeepsDetection)
{
    const std::filesystem::path dir = makeDataDir("no-validate");

    PluginOptions options;
    options.hedrVersion = ESM::VER_100;
    options.master = false;
    options.masters = { { "NotChecked.esm", 1 } };
    writeInDir(dir, "NoValidate.esp", buildPlugin(options));

    const std::vector<std::string> contentFiles = { "NoValidate.esp" };
    const Files::Collections collections({ dir });

    Oblivion::Configuration noValidate;
    noValidate.mValidate = false;
    noValidate.mVerbose = true;
    const Oblivion::ContentReport report = Oblivion::checkContentFiles(contentFiles, collections, noValidate);
    EXPECT_EQ(report.mOblivionContentCount, 1u);
    expectNoErrors(report);
    EXPECT_TRUE(hasMessageContaining(report, "validation disabled")) << issueSummary(report);
}
