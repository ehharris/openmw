// Oblivion content loading system tests.
//
// These tests build synthetic Oblivion plugin files in a temporary directory and load
// them with Oblivion::ContentLoader (components/oblivion/contentloader.{hpp,cpp}).
// The file lives at the path requested by the task brief; it is registered in the
// components-tests target (see apps/components_tests/CMakeLists.txt), which is where
// all component unit tests in this repository are built and run.

#include <components/oblivion/contentloader.hpp>

#include <components/esm/common.hpp>
#include <components/esm/fourcc.hpp>
#include <components/files/collections.hpp>
#include <components/files/oblivion/esm.hpp>
#include <components/testing/util.hpp>

#include <gtest/gtest.h>

#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <string>
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

    std::filesystem::path makeTempFile(const std::string& name, const std::vector<char>& bytes)
    {
        const std::filesystem::path path = TestingOpenMW::outputFilePath(name);
        writeFile(path, bytes);
        return path;
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
        xlcl.u32(0); // quad flags, high bits unused
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
        // A minimal REFR record; only its type and formId matter for these tests.
        return makeRecord(Files::Oblivion::REC_REFR, /*flags=*/0, formId, /*versionControlInfo=*/0,
            std::vector<char>{});
    }

    // Builds a plugin with a WRLD group tree (WRLD record plus its worldspace child groups
    // holding one exterior CELL and one REFR) and an interior CELL in a RecordType group.
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
}

TEST(OblivionContentLoader, isOblivionPluginRecognisesOblivionFiles)
{
    PluginOptions options;
    options.hedrVersion = ESM::VER_080; // pre-1.1 plugins
    const std::filesystem::path oldPlugin = makeTempFile("oldhed.esm", buildPlugin(options));
    EXPECT_TRUE(Oblivion::ContentLoader::isOblivionPlugin(oldPlugin));

    options.hedrVersion = ESM::VER_100; // vanilla Oblivion
    const std::filesystem::path vanillaPlugin = makeTempFile("vanilla.esm", buildPlugin(options));
    EXPECT_TRUE(Oblivion::ContentLoader::isOblivionPlugin(vanillaPlugin));

    options.hedrVersion = ESM::VER_094; // Fallout 3 / Skyrim share the TES4 signature
    const std::filesystem::path fo3Plugin = makeTempFile("fo3.esm", buildPlugin(options));
    EXPECT_FALSE(Oblivion::ContentLoader::isOblivionPlugin(fo3Plugin));

    options.hedrVersion = ESM::VER_120; // Morrowind
    const std::filesystem::path tes3Plugin = makeTempFile("tes3.esm", buildPlugin(options));
    EXPECT_FALSE(Oblivion::ContentLoader::isOblivionPlugin(tes3Plugin));

    const std::filesystem::path garbage = makeTempFile("garbage.esm", { 'N', 'O', 'P', 'E' });
    EXPECT_FALSE(Oblivion::ContentLoader::isOblivionPlugin(garbage));

    const std::filesystem::path missing = TestingOpenMW::outputFilePath("does-not-exist.esm");
    EXPECT_FALSE(Oblivion::ContentLoader::isOblivionPlugin(missing));
}

TEST(OblivionContentLoader, validateAcceptsOblivionMaster)
{
    PluginOptions options;
    options.hedrVersion = ESM::VER_100;
    options.master = true;
    options.author = "Test Author";
    options.description = "Test Description";

    const std::filesystem::path path = makeTempFile("validated.esm", buildPlugin(options));

    const Files::Oblivion::Header header = Oblivion::ContentLoader::validate(path);
    EXPECT_TRUE(header.isMaster());
    EXPECT_EQ(header.mData.version.ui, ESM::VER_100);
    EXPECT_EQ(header.mAuthor, "Test Author");
    EXPECT_EQ(header.mDesc, "Test Description");
}

TEST(OblivionContentLoader, validateRejectsNonOblivionPlugins)
{
    PluginOptions options;
    options.hedrVersion = ESM::VER_094; // Fallout 3: TES4 signature, but not an Oblivion HEDR version
    const std::filesystem::path fo3Plugin = makeTempFile("not-oblivion.esm", buildPlugin(options));
    EXPECT_THROW(Oblivion::ContentLoader::validate(fo3Plugin), std::runtime_error);

    const std::filesystem::path truncated = makeTempFile("truncated.esm", { 'T', 'E', 'S' });
    EXPECT_THROW(Oblivion::ContentLoader::validate(truncated), std::runtime_error);
}

TEST(OblivionContentLoader, loadReadsPluginHeaderAndMasters)
{
    PluginOptions options;
    options.hedrVersion = ESM::VER_080;
    options.master = false; // .esp plugins are not flagged as masters
    options.numRecords = 1234;
    options.nextObjectId = 0x00000abc;
    options.masters = { { "Oblivion.esm", 123456789 }, { "Update.esm", 987654321 } };

    const std::filesystem::path path = makeTempFile("header.esp", buildPlugin(options));

    const Oblivion::ContentFile content = Oblivion::ContentLoader().load(path);
    EXPECT_FALSE(content.mMaster); // determined by the flag, not by the file extension
    EXPECT_EQ(content.mHedrVersion, ESM::VER_080);
    EXPECT_EQ(content.mRecordCount, 1234);
    EXPECT_EQ(content.mNextObjectId, 0x00000abcu);
    EXPECT_EQ(content.mAuthor, "Bethesda Game Studios");
    EXPECT_EQ(content.mDescription, "Test plugin");

    ASSERT_EQ(content.mMasters.size(), 2u);
    EXPECT_EQ(content.mMasters[0].mName, "Oblivion.esm");
    EXPECT_EQ(content.mMasters[0].mSize, 123456789u);
    EXPECT_EQ(content.mMasters[1].mName, "Update.esm");
    EXPECT_EQ(content.mMasters[1].mSize, 987654321u);
}

TEST(OblivionContentLoader, loadExtractsWorldData)
{
    PluginOptions options;
    const std::filesystem::path path
        = makeTempFile("worlds.esm", buildPlugin(options, buildWorldCellContent()));

    const Oblivion::ContentFile content = Oblivion::ContentLoader().load(path);

    ASSERT_EQ(content.mWorlds.size(), 1u);
    const Oblivion::WorldData& world = content.mWorlds.front();
    EXPECT_EQ(world.mFormId, 0x00000010u);
    EXPECT_EQ(world.mEditorId, "Tamriel");
    EXPECT_EQ(world.mFullName, "Tamriel");
    EXPECT_EQ(world.mWorldFlags, Oblivion::kWorldFlagOblivion);
    EXPECT_EQ(world.mClimate, 0x00000040u);
    EXPECT_EQ(world.mWater, 0x00000041u);
    EXPECT_EQ(world.mMapFile, "textures/terrain/tamriel.dds");
    EXPECT_FLOAT_EQ(world.mLandLevel, -200.f);
    EXPECT_FLOAT_EQ(world.mWaterLevel, 0.f);

    // The exterior cell is attributed to its world space through the group hierarchy
    ASSERT_EQ(world.mCells.size(), 1u);
    EXPECT_EQ(world.mCells.front(), 0x00000020u);

    const Oblivion::WorldData* found = content.findWorld(0x00000010u);
    ASSERT_NE(found, nullptr);
    EXPECT_EQ(found->mFormId, 0x00000010u);
    EXPECT_EQ(content.findWorld(0xdeadbeefu), nullptr);
}

TEST(OblivionContentLoader, loadExtractsCellData)
{
    PluginOptions options;
    const std::filesystem::path path
        = makeTempFile("cells.esm", buildPlugin(options, buildWorldCellContent()));

    const Oblivion::ContentFile content = Oblivion::ContentLoader().load(path);

    ASSERT_EQ(content.mCells.size(), 2u);

    ASSERT_EQ(content.mCells[0].mFormId, 0x00000020u); // file order: exterior cell comes first
    const Oblivion::CellData& exterior = content.mCells[0];
    EXPECT_TRUE(exterior.isExterior());
    EXPECT_EQ(exterior.mEditorId, "TamrielExterior");
    EXPECT_EQ(exterior.mX, -7);
    EXPECT_EQ(exterior.mY, 3);
    EXPECT_FLOAT_EQ(exterior.mWaterHeight, 4.5f);
    EXPECT_EQ(exterior.mWorld, 0x00000010u);

    ASSERT_EQ(content.mCells[1].mFormId, 0x00000030u);
    const Oblivion::CellData& interior = content.mCells[1];
    EXPECT_FALSE(interior.isExterior());
    EXPECT_EQ(interior.mEditorId, "Prison");
    EXPECT_EQ(interior.mFullName, "Imperial City Prison");
    EXPECT_EQ(interior.mWorld, 0u); // interior cells do not belong to a world space

    const Oblivion::CellData* found = content.findCell(0x00000030u);
    ASSERT_NE(found, nullptr);
    EXPECT_EQ(content.findCell(0xdeadbeefu), nullptr);

    const std::vector<const Oblivion::CellData*> worldCells = content.getCells(0x00000010u);
    ASSERT_EQ(worldCells.size(), 1u);
    EXPECT_EQ(worldCells.front()->mFormId, 0x00000020u);
    EXPECT_TRUE(content.getCells(0xdeadbeefu).empty());
}

TEST(OblivionContentLoader, loadCountsRecordsPerType)
{
    PluginOptions options;
    const std::filesystem::path path
        = makeTempFile("counts.esm", buildPlugin(options, buildWorldCellContent()));

    const Oblivion::ContentFile content = Oblivion::ContentLoader().load(path);

    EXPECT_EQ(content.getRecordCount(Files::Oblivion::REC_WRLD), 1u);
    EXPECT_EQ(content.getRecordCount(Files::Oblivion::REC_CELL), 2u);
    EXPECT_EQ(content.getRecordCount(Files::Oblivion::REC_REFR), 1u);
    EXPECT_EQ(content.getRecordCount(Files::Oblivion::REC_SPEL), 0u);
    EXPECT_EQ(content.getTotalRecordCount(), 4u); // 1 WRLD + 2 CELL + 1 REFR (groups excluded)
}

TEST(OblivionContentLoader, findMissingMastersAgainstFileList)
{
    PluginOptions options;
    options.masters = { { "Oblivion.esm", 1 }, { "Update.esm", 2 } };
    const std::filesystem::path path = makeTempFile("missing.esm", buildPlugin(options));

    const Oblivion::ContentFile content = Oblivion::ContentLoader().load(path);

    EXPECT_TRUE(Oblivion::ContentLoader::findMissingMasters(content, { "Oblivion.esm", "Update.esm" }).empty());
    // Master names are matched case-insensitively
    EXPECT_TRUE(Oblivion::ContentLoader::findMissingMasters(content, { "oblivion.ESM", "update.esm" }).empty());
    const std::vector<std::string> missing = Oblivion::ContentLoader::findMissingMasters(content, { "Oblivion.esm" });
    ASSERT_EQ(missing.size(), 1u);
    EXPECT_EQ(missing.front(), "Update.esm");
    EXPECT_EQ(Oblivion::ContentLoader::findMissingMasters(content, std::vector<std::string>{}).size(), 2u);
}

TEST(OblivionContentLoader, findMissingMastersAgainstCollections)
{
    const std::filesystem::path dir = TestingOpenMW::outputDir();
    writeFile(dir / "Oblivion.esm", { 'f', 'a', 'k', 'e' });

    PluginOptions options;
    options.masters = { { "Oblivion.esm", 1 }, { "NoSuchMaster.esm", 2 } };
    const std::filesystem::path path = makeTempFile("collections.esp", buildPlugin(options));

    const Oblivion::ContentFile content = Oblivion::ContentLoader().load(path);
    const Files::Collections collections({ dir });

    const std::vector<std::string> missing = Oblivion::ContentLoader::findMissingMasters(content, collections);
    ASSERT_EQ(missing.size(), 1u);
    EXPECT_EQ(missing.front(), "NoSuchMaster.esm");
}

TEST(OblivionContentLoader, loadRejectsNonOblivionPlugin)
{
    PluginOptions options;
    options.hedrVersion = ESM::VER_094; // Fallout 3
    const std::filesystem::path path = makeTempFile("fo3-load.esm", buildPlugin(options));
    EXPECT_THROW(Oblivion::ContentLoader().load(path), std::runtime_error);
}
