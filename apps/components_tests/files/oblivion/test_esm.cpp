// Oblivion (TES4) ESM/ESP parser tests.
//
// These tests build synthetic Oblivion plugin files in a temporary directory and parse
// them with components/files/oblivion/esm.{hpp,cpp}. The component test suite lives in
// apps/components_tests, so this file is registered there (see CMakeLists.txt next to
// this file's parent directory).

#include <components/files/oblivion/esm.hpp>

#include <components/esm/common.hpp>
#include <components/esm/fourcc.hpp>
#include <components/testing/util.hpp>

#include <gtest/gtest.h>

#include <zlib.h>

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
        void u64(std::uint64_t value)
        {
            for (int shift = 0; shift < 64; shift += 8)
                u8(static_cast<std::uint8_t>((value >> shift) & 0xff));
        }
        void floatBits(std::uint32_t bits) { u32(bits); }
        void fourcc(std::uint32_t value) { u32(value); }
        void raw(const char* data, std::size_t size) { bytes.insert(bytes.end(), data, data + size); }
        void raw(const std::string& str) { raw(str.data(), str.size()); }
        void bytesFrom(const std::vector<char>& other) { bytes.insert(bytes.end(), other.begin(), other.end()); }
    };

    std::vector<char> makeSubRecord(std::uint32_t type, const std::vector<char>& payload)
    {
        BytesBuilder builder;
        builder.fourcc(type);
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
        builder.fourcc(type);
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
        builder.fourcc(ESM::fourCC("GRUP"));
        builder.u32(static_cast<std::uint32_t>(20 + content.size()));
        builder.u32(label);
        builder.i32(type);
        builder.u32(0); // stamp
        builder.bytesFrom(content);
        return std::move(builder.bytes);
    }

    void writePlugin(const std::filesystem::path& path, const std::vector<char>& bytes)
    {
        std::ofstream stream(path, std::ios::binary);
        stream.exceptions(std::ofstream::failbit | std::ofstream::badbit);
        stream.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
    }

    struct PluginOptions
    {
        std::uint32_t hedrVersion = ESM::VER_080;
        bool master = true; // set the Rec_ESM flag on the TES4 record
        std::uint32_t numRecords = 0;
        std::uint32_t nextObjectId = 0x00000014;
        std::string author = "Bethesda Game Studios";
        std::string description = "Test plugin";
        std::vector<std::pair<std::string, std::uint64_t>> masters;
        std::vector<std::uint32_t> overrides; // ONAM
    };

    // Builds a synthetic Oblivion ESM/ESP with the given TES4 header and extra content
    // (raw, already serialized records/groups) appended after the TES4 record.
    std::vector<char> buildPlugin(const PluginOptions& options, const std::vector<char>& content = {})
    {
        std::vector<char> hedrPayload;
        {
            BytesBuilder hedr;
            hedr.floatBits(options.hedrVersion);
            hedr.i32(static_cast<std::int32_t>(options.numRecords));
            hedr.u32(options.nextObjectId);
            hedrPayload = makeSubRecord(ESM::fourCC("HEDR"), hedr.bytes);
        }

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
            data.u64(master.second);
            payload.bytesFrom(makeSubRecord(ESM::fourCC("DATA"), data.bytes));
        }
        if (!options.overrides.empty())
        {
            BytesBuilder onam;
            for (std::uint32_t overrideId : options.overrides)
                onam.u32(overrideId);
            payload.bytesFrom(makeSubRecord(ESM::fourCC("ONAM"), onam.bytes));
        }

        const std::uint32_t flags = options.master ? static_cast<std::uint32_t>(Files::Oblivion::Rec_ESM) : 0u;
        const std::vector<char> tes4 = makeRecord(
            Files::Oblivion::REC_TES4, flags, /*formId=*/0, /*versionControlInfo=*/0, payload.bytes);

        std::vector<char> plugin = tes4;
        plugin.insert(plugin.end(), content.begin(), content.end());
        return plugin;
    }

    std::filesystem::path makeTempPlugin(const std::string& name, const std::vector<char>& bytes)
    {
        const std::filesystem::path path = TestingOpenMW::outputFilePath(name);
        writePlugin(path, bytes);
        return path;
    }

    std::vector<char> makeSpellRecordPayload(const std::string& editorId, const std::string& spellName,
        std::uint32_t spellType, std::uint32_t cost, std::uint32_t level, std::uint32_t flags,
        const std::vector<std::pair<std::uint32_t, Files::Oblivion::EffectItem>>& effects)
    {
        BytesBuilder payload;
        payload.bytesFrom(makeStringSubRecord(ESM::fourCC("EDID"), editorId));
        payload.bytesFrom(makeStringSubRecord(ESM::fourCC("FULL"), spellName));

        BytesBuilder spit;
        spit.u32(spellType);
        spit.u32(cost);
        spit.u32(level);
        spit.u32(flags);
        payload.bytesFrom(makeSubRecord(ESM::fourCC("SPIT"), spit.bytes));

        for (const auto& [effectId, effect] : effects)
        {
            BytesBuilder efid;
            efid.fourcc(effectId);
            payload.bytesFrom(makeSubRecord(ESM::fourCC("EFID"), efid.bytes));

            BytesBuilder efit;
            efit.fourcc(effectId);
            efit.u32(effect.magnitude);
            efit.u32(effect.area);
            efit.u32(effect.duration);
            efit.u32(effect.deliveryType);
            efit.u32(effect.actorValue);
            payload.bytesFrom(makeSubRecord(ESM::fourCC("EFIT"), efit.bytes));
        }

        return std::move(payload.bytes);
    }
}

TEST(FilesOblivionEsm, isOblivionHedrVersionRecognisesOblivionVersions)
{
    EXPECT_TRUE(Files::Oblivion::isOblivionHedrVersion(ESM::VER_080)); // 0.8, pre-1.1 plugins
    EXPECT_TRUE(Files::Oblivion::isOblivionHedrVersion(ESM::VER_100)); // 1.0, vanilla Oblivion.esm

    EXPECT_FALSE(Files::Oblivion::isOblivionHedrVersion(ESM::VER_120)); // 1.2 is Morrowind's HEDR
    EXPECT_FALSE(Files::Oblivion::isOblivionHedrVersion(ESM::VER_130)); // 1.3 is Morrowind's HEDR
    EXPECT_FALSE(Files::Oblivion::isOblivionHedrVersion(ESM::VER_094)); // Fallout 3 / Skyrim
    EXPECT_FALSE(Files::Oblivion::isOblivionHedrVersion(ESM::VER_095)); // Fallout 4
    EXPECT_FALSE(Files::Oblivion::isOblivionHedrVersion(ESM::VER_170)); // Skyrim
    EXPECT_FALSE(Files::Oblivion::isOblivionHedrVersion(0));
}

TEST(FilesOblivionEsm, isKnownGroupTypeAndRecordType)
{
    EXPECT_TRUE(Files::Oblivion::isKnownGroupType(Files::Oblivion::Grp_RecordType));
    EXPECT_TRUE(Files::Oblivion::isKnownGroupType(Files::Oblivion::Grp_ExteriorCellSubBlock));
    EXPECT_TRUE(Files::Oblivion::isKnownGroupType(Files::Oblivion::Grp_CellVisibleDistantChild));
    EXPECT_FALSE(Files::Oblivion::isKnownGroupType(11));
    EXPECT_FALSE(Files::Oblivion::isKnownGroupType(-1));

    EXPECT_TRUE(Files::Oblivion::isOblivionRecordType(Files::Oblivion::REC_SPEL));
    EXPECT_TRUE(Files::Oblivion::isOblivionRecordType(Files::Oblivion::REC_ENCH));
    EXPECT_TRUE(Files::Oblivion::isOblivionRecordType(Files::Oblivion::REC_PGRD)); // Oblivion-only
    EXPECT_TRUE(Files::Oblivion::isOblivionRecordType(Files::Oblivion::REC_ROAD)); // Oblivion-only
    EXPECT_TRUE(Files::Oblivion::isOblivionRecordType(Files::Oblivion::REC_SBSP)); // Oblivion-only
    EXPECT_TRUE(Files::Oblivion::isOblivionRecordType(Files::Oblivion::REC_ACRE)); // Oblivion-only
    EXPECT_TRUE(Files::Oblivion::isOblivionRecordType(Files::Oblivion::REC_NPC_)); // trailing underscore
    EXPECT_FALSE(Files::Oblivion::isOblivionRecordType(ESM::fourCC("XXXX")));
}

TEST(FilesOblivionEsm, parsesMinimalMasterHeader)
{
    PluginOptions options;
    options.hedrVersion = ESM::VER_080;
    options.numRecords = 42;
    options.nextObjectId = 0x00012345;
    options.author = "Bethesda Game Studios";
    options.description = "Oblivion.esm";
    options.masters = { { "Tribunal.esm", 1234567 }, { "Bloodmoon.esm", 2345678 } };

    const std::filesystem::path path = makeTempPlugin("header.esm", buildPlugin(options));

    Files::Oblivion::EsmFile plugin(path);
    ASSERT_TRUE(plugin.isOpen());

    const Files::Oblivion::Header& header = plugin.header();
    EXPECT_TRUE(header.isMaster()); // Rec_ESM flag is set
    EXPECT_EQ(header.flags & Files::Oblivion::Rec_ESM, Files::Oblivion::Rec_ESM);
    EXPECT_FLOAT_EQ(header.mData.version.f, 0.8f); // VER_080
    EXPECT_EQ(header.mData.records, 42);
    EXPECT_EQ(header.mData.nextObjectId, 0x00012345u);
    EXPECT_EQ(header.mAuthor, "Bethesda Game Studios");
    EXPECT_EQ(header.mDesc, "Oblivion.esm");

    ASSERT_EQ(header.mMaster.size(), 2u);
    EXPECT_EQ(header.mMaster[0].name, "Tribunal.esm");
    EXPECT_EQ(header.mMaster[0].size, 1234567u);
    EXPECT_EQ(header.mMaster[1].name, "Bloodmoon.esm");
    EXPECT_EQ(header.mMaster[1].size, 2345678u);

    EXPECT_TRUE(plugin.records().empty()); // parse() has not been called yet
    plugin.parse();
    EXPECT_TRUE(plugin.records().empty()); // no records in the file
    EXPECT_TRUE(plugin.groups().empty());
}

TEST(FilesOblivionEsm, distinguishesPluginFromMaster)
{
    PluginOptions options;
    options.master = false;
    options.description = ""; // description is optional
    options.author = ""; // author is optional

    const std::filesystem::path path = makeTempPlugin("plugin.esp", buildPlugin(options));

    Files::Oblivion::EsmFile plugin(path);
    EXPECT_FALSE(plugin.header().isMaster());
    EXPECT_EQ(plugin.header().mDesc, "");
    EXPECT_EQ(plugin.header().mAuthor, "");
}

TEST(FilesOblivionEsm, acceptsVersionOnePointZeroHeader)
{
    PluginOptions options;
    options.hedrVersion = ESM::VER_100; // what vanilla Oblivion writes

    const std::filesystem::path path = makeTempPlugin("vanilla.esm", buildPlugin(options));

    Files::Oblivion::EsmFile plugin(path);
    EXPECT_FLOAT_EQ(plugin.header().mData.version.f, 1.0f);
}

TEST(FilesOblivionEsm, ignoresWhitespaceOnlyMasterEntries)
{
    // Oblivion.esm contains a master entry whose name is only whitespace (a single space).
    // Such entries must not surface in the parsed master list, otherwise content
    // validation reports them as missing masters. Their DATA size record belongs to the
    // ignored entry and must not be attributed to another master either.
    PluginOptions options;
    options.masters = { { " ", 123456 }, { "Oblivion.esm", 2345678 }, { "", 0 } };

    const std::filesystem::path path = makeTempPlugin("whitespace-masters.esm", buildPlugin(options));

    Files::Oblivion::EsmFile plugin(path);
    ASSERT_TRUE(plugin.isOpen());

    const std::vector<ESM::MasterData>& masters = plugin.header().mMaster;
    ASSERT_EQ(masters.size(), 1u);
    EXPECT_EQ(masters[0].name, "Oblivion.esm");
    EXPECT_EQ(masters[0].size, 2345678u);
}

TEST(FilesOblivionEsm, noMastersWhenAllAreWhitespace)
{
    // A plugin whose only master is whitespace-only ends up with an empty master list;
    // the DATA record following the ignored MAST must not throw either.
    PluginOptions options;
    options.masters = { { " ", 42 } };

    const std::filesystem::path path = makeTempPlugin("only-whitespace-masters.esm", buildPlugin(options));

    Files::Oblivion::EsmFile plugin(path);
    EXPECT_TRUE(plugin.header().mMaster.empty());
}

TEST(FilesOblivionEsm, toleratesWhitespaceOnlyDescription)
{
    // The TES4 record of Oblivion.esm carries a SNAM subrecord consisting of a single
    // space (" \0"). It is the file description, not a master: it must parse fine and
    // must never end up in the master list.
    PluginOptions options;
    options.description = " ";

    const std::filesystem::path path = makeTempPlugin("whitespace-desc.esm", buildPlugin(options));

    Files::Oblivion::EsmFile plugin(path);
    EXPECT_EQ(plugin.header().mDesc, " ");
    EXPECT_TRUE(plugin.header().mMaster.empty());
}

TEST(FilesOblivionEsm, rejectsNonOblivionFiles)
{
    const std::filesystem::path path = makeTempPlugin("morrowind.esm", {});
    {
        std::ofstream stream(path, std::ios::binary | std::ios::trunc);
        stream << "TES3fake";
    }

    EXPECT_THROW(Files::Oblivion::EsmFile{ path }, std::runtime_error);
}

TEST(FilesOblivionEsm, rejectsFilesTooSmallForHeader)
{
    const std::filesystem::path path = makeTempPlugin("tiny.esm", std::vector<char>{ 'T', 'E', 'S' });

    EXPECT_THROW(Files::Oblivion::EsmFile{ path }, std::runtime_error);
}

TEST(FilesOblivionEsm, rejectsTruncatedFile)
{
    PluginOptions options;
    std::vector<char> bytes = buildPlugin(options);
    bytes.pop_back(); // corrupt the header record payload

    const std::filesystem::path path = makeTempPlugin("truncated.esm", bytes);

    EXPECT_THROW(Files::Oblivion::EsmFile{ path }, std::runtime_error);
}

TEST(FilesOblivionEsm, rejectsHedrWithWrongSize)
{
    std::vector<char> hedrPayload = makeSubRecord(ESM::fourCC("HEDR"), { 1, 2, 3 }); // not 12 bytes

    BytesBuilder payload;
    payload.bytesFrom(hedrPayload);
    const std::vector<char> tes4 = makeRecord(Files::Oblivion::REC_TES4, Files::Oblivion::Rec_ESM, 0, 0, payload.bytes);

    const std::filesystem::path path = makeTempPlugin("badhedr.esm", tes4);

    EXPECT_THROW(Files::Oblivion::EsmFile{ path }, std::runtime_error);
}

TEST(FilesOblivionEsm, parsesRecordsGroupsAndFormIds)
{
    PluginOptions options;
    options.numRecords = 3;
    options.masters = { { "Oblivion.esm", 111111 } };

    Files::Oblivion::EffectItem effect{};
    effect.magnitude = 10;
    effect.area = 0;
    effect.duration = 30;
    effect.deliveryType = Files::Oblivion::Delivery_Touch;
    effect.actorValue = 0;

    const std::vector<char> spelPayload = makeSpellRecordPayload("TestFireball", "Test Fireball",
        Files::Oblivion::Spell_Spell, 25, Files::Oblivion::Level_Apprentice, Files::Oblivion::Spell_ManualCost,
        { { ESM::fourCC("FIDG"), effect } });

    const std::vector<char> spelRecord = makeRecord(
        Files::Oblivion::REC_SPEL, /*flags=*/0, /*formId=*/0x00000123, /*versionControlInfo=*/0x11223344, spelPayload);

    const std::vector<char> spelGroup = makeGroup(ESM::fourCC("SPEL"), Files::Oblivion::Grp_RecordType, spelRecord);

    const std::vector<char> content = spelGroup;
    const std::filesystem::path path = makeTempPlugin("records.esp", buildPlugin(options, content));

    Files::Oblivion::EsmFile plugin(path);
    plugin.parse();

    ASSERT_EQ(plugin.records().size(), 1u);
    const Files::Oblivion::Record& record = plugin.records().front();
    EXPECT_EQ(record.typeId, Files::Oblivion::REC_SPEL);
    EXPECT_EQ(record.formId, 0x00000123u);
    EXPECT_EQ(record.versionControlInfo, 0x11223344u);
    EXPECT_EQ(record.flags, 0u);

    ASSERT_EQ(plugin.groups().size(), 1u);
    const Files::Oblivion::Group& group = plugin.groups().front();
    EXPECT_EQ(group.type, Files::Oblivion::Grp_RecordType);
    EXPECT_EQ(group.label, ESM::fourCC("SPEL"));
    EXPECT_EQ(group.groupSize, spelGroup.size());
    EXPECT_EQ(group.depth, 0u);
}

TEST(FilesOblivionEsm, parsesNestedGroupsWithDepth)
{
    PluginOptions options;

    const std::vector<char> refrRecord = makeRecord(Files::Oblivion::REC_REFR, Files::Oblivion::Rec_Disabled,
        /*formId=*/0x000ff001, /*versionControlInfo=*/0, std::vector<char>{});

    const std::vector<char> cellChild = makeGroup(/*label=*/0x000ff100, Files::Oblivion::Grp_CellChild, refrRecord);
    const std::vector<char> subBlock = makeGroup(/*label=*/0, Files::Oblivion::Grp_ExteriorCellSubBlock, cellChild);
    const std::vector<char> block = makeGroup(/*label=*/0, Files::Oblivion::Grp_ExteriorCellBlock, subBlock);

    const std::filesystem::path path = makeTempPlugin("nested.esp", buildPlugin(options, block));

    Files::Oblivion::EsmFile plugin(path);
    plugin.parse();

    ASSERT_EQ(plugin.groups().size(), 3u);
    EXPECT_EQ(plugin.groups()[0].type, Files::Oblivion::Grp_ExteriorCellBlock);
    EXPECT_EQ(plugin.groups()[0].depth, 0u);
    EXPECT_EQ(plugin.groups()[1].type, Files::Oblivion::Grp_ExteriorCellSubBlock);
    EXPECT_EQ(plugin.groups()[1].depth, 1u);
    EXPECT_EQ(plugin.groups()[2].type, Files::Oblivion::Grp_CellChild);
    EXPECT_EQ(plugin.groups()[2].depth, 2u);

    ASSERT_EQ(plugin.records().size(), 1u);
    EXPECT_EQ(plugin.records().front().formId, 0x000ff001u);
    EXPECT_TRUE(plugin.records().front().hasFlag(Files::Oblivion::Rec_Disabled));
}

TEST(FilesOblivionEsm, rejectsTruncatedRecordHeaderAtEof)
{
    PluginOptions options;
    std::vector<char> bytes = buildPlugin(options);
    bytes.insert(bytes.end(), { 'A', 'B', 'C' }); // trailing garbage, smaller than a record header

    const std::filesystem::path path = makeTempPlugin("garbage.esp", bytes);

    Files::Oblivion::EsmFile plugin(path);
    EXPECT_THROW(plugin.parse(), std::runtime_error);
}

TEST(FilesOblivionEsm, rejectsRecordExtendingPastGroup)
{
    PluginOptions options;

    const std::vector<char> refrRecord = makeRecord(Files::Oblivion::REC_REFR, 0, 0x000ff001, 0, std::vector<char>{});
    // Group size claims the group is smaller than the record it contains
    BytesBuilder group;
    group.fourcc(ESM::fourCC("GRUP"));
    group.u32(20 + 10); // groupSize, far smaller than the nested record
    group.u32(0);
    group.i32(Files::Oblivion::Grp_RecordType);
    group.u32(0);
    group.bytesFrom(refrRecord);

    const std::filesystem::path path = makeTempPlugin("badgroup.esp", buildPlugin(options, group.bytes));

    Files::Oblivion::EsmFile plugin(path);
    EXPECT_THROW(plugin.parse(), std::runtime_error);
}

TEST(FilesOblivionEsm, rejectsGroupSmallerThanHeader)
{
    PluginOptions options;

    BytesBuilder group;
    group.fourcc(ESM::fourCC("GRUP"));
    group.u32(8); // groupSize < 20
    group.u32(0);
    group.i32(Files::Oblivion::Grp_RecordType);
    group.u32(0);

    const std::filesystem::path path = makeTempPlugin("tinygroup.esp", buildPlugin(options, group.bytes));

    Files::Oblivion::EsmFile plugin(path);
    EXPECT_THROW(plugin.parse(), std::runtime_error);
}

TEST(FilesOblivionEsm, handlesCompressedRecords)
{
    PluginOptions options;

    // Build a record payload, then compress it and prefix it with the uncompressed size
    const std::vector<char> spellPayload = makeSpellRecordPayload("CompressedSpell", "Compressed Spell",
        Files::Oblivion::Spell_Ability, 0, Files::Oblivion::Level_Novice, 0, {});

    std::vector<char> compressed;
    {
        uLongf bound = compressBound(static_cast<uLong>(spellPayload.size()));
        compressed.resize(bound);
        uLongf compressedSize = bound;
        ASSERT_EQ(compress(reinterpret_cast<Bytef*>(compressed.data()), &compressedSize,
                       reinterpret_cast<const Bytef*>(spellPayload.data()), spellPayload.size()),
            Z_OK);
        compressed.resize(compressedSize);
    }

    BytesBuilder payload;
    payload.u32(static_cast<std::uint32_t>(spellPayload.size()));
    payload.bytesFrom(compressed);

    const std::vector<char> record = makeRecord(Files::Oblivion::REC_SPEL, Files::Oblivion::Rec_Compressed,
        /*formId=*/0x00000abc, /*versionControlInfo=*/0, payload.bytes);
    const std::vector<char> groupData = makeGroup(ESM::fourCC("SPEL"), Files::Oblivion::Grp_RecordType, record);

    const std::filesystem::path path = makeTempPlugin("compressed.esp", buildPlugin(options, groupData));

    Files::Oblivion::EsmFile plugin(path);
    plugin.parse();

    ASSERT_EQ(plugin.records().size(), 1u);
    const Files::Oblivion::Record& parsed = plugin.records().front();
    EXPECT_TRUE(parsed.hasFlag(Files::Oblivion::Rec_Compressed));

    const Files::Oblivion::Spell spell = Files::Oblivion::decodeSpell(parsed);
    EXPECT_EQ(spell.editorId, "CompressedSpell");
    EXPECT_EQ(spell.name, "Compressed Spell");
    EXPECT_EQ(spell.data.type, Files::Oblivion::Spell_Ability);
}

TEST(FilesOblivionEsm, handlesCompressedRecordInsideGroup)
{
    PluginOptions options;

    const std::vector<char> spellPayload = makeSpellRecordPayload("NestedCompressed", "Nested Compressed",
        Files::Oblivion::Spell_Spell, 0, Files::Oblivion::Level_Master, 0, {});

    std::vector<char> compressed;
    {
        uLongf bound = compressBound(static_cast<uLong>(spellPayload.size()));
        compressed.resize(bound);
        uLongf compressedSize = bound;
        ASSERT_EQ(compress(reinterpret_cast<Bytef*>(compressed.data()), &compressedSize,
                       reinterpret_cast<const Bytef*>(spellPayload.data()), spellPayload.size()),
            Z_OK);
        compressed.resize(compressedSize);
    }

    BytesBuilder payload;
    payload.u32(static_cast<std::uint32_t>(spellPayload.size()));
    payload.bytesFrom(compressed);

    // Group size must account for the *compressed* (on-disk) size of the record
    const std::vector<char> record = makeRecord(Files::Oblivion::REC_SPEL, Files::Oblivion::Rec_Compressed,
        /*formId=*/0x00000abd, /*versionControlInfo=*/0, payload.bytes);
    const std::vector<char> groupData = makeGroup(ESM::fourCC("SPEL"), Files::Oblivion::Grp_RecordType, record);

    const std::filesystem::path path = makeTempPlugin("nestedcompressed.esp", buildPlugin(options, groupData));

    Files::Oblivion::EsmFile plugin(path);
    plugin.parse();

    ASSERT_EQ(plugin.groups().size(), 1u);
    ASSERT_EQ(plugin.records().size(), 1u);

    const Files::Oblivion::Spell spell = Files::Oblivion::decodeSpell(plugin.records().front());
    EXPECT_EQ(spell.editorId, "NestedCompressed");
    EXPECT_EQ(spell.data.level, Files::Oblivion::Level_Master);
}

TEST(FilesOblivionEsm, rejectsCorruptCompressedRecord)
{
    PluginOptions options;

    BytesBuilder payload;
    payload.u32(1000); // uncompressed size, but nothing to decompress
    payload.u32(0xdeadbeef);

    const std::vector<char> record = makeRecord(Files::Oblivion::REC_SPEL, Files::Oblivion::Rec_Compressed,
        /*formId=*/0x00000abd, /*versionControlInfo=*/0, payload.bytes);

    const std::filesystem::path path = makeTempPlugin("badcompress.esp", buildPlugin(options, record));

    Files::Oblivion::EsmFile plugin(path);
    EXPECT_THROW(plugin.parse(), std::runtime_error);
}

TEST(FilesOblivionEsm, decodesSpellWithScriptEffect)
{
    Files::Oblivion::EffectItem fireball{};
    fireball.magnitude = 55;
    fireball.area = 3;
    fireball.duration = 7;
    fireball.deliveryType = Files::Oblivion::Delivery_Target;
    fireball.actorValue = 42;

    BytesBuilder effectPayload;
    // First effect: regular fireball
    std::vector<char> payload = makeSpellRecordPayload("FireBallScript", "Fireball Script",
        Files::Oblivion::Spell_Spell, 200, Files::Oblivion::Level_Expert, Files::Oblivion::Spell_AreaEffectIgnoresLOS,
        { { ESM::fourCC("FIDG"), fireball } });

    // Second effect: script effect with a script attached
    BytesBuilder extra;
    BytesBuilder efid;
    efid.fourcc(ESM::fourCC("SCPF"));
    extra.bytesFrom(makeSubRecord(ESM::fourCC("EFID"), efid.bytes));

    Files::Oblivion::EffectItem scriptEffect{};
    scriptEffect.magnitude = 1;
    scriptEffect.area = 0;
    scriptEffect.duration = 0;
    scriptEffect.deliveryType = Files::Oblivion::Delivery_Self;
    scriptEffect.actorValue = 0;

    BytesBuilder efit;
    efit.fourcc(ESM::fourCC("SCPF"));
    efit.u32(scriptEffect.magnitude);
    efit.u32(scriptEffect.area);
    efit.u32(scriptEffect.duration);
    efit.u32(scriptEffect.deliveryType);
    efit.u32(scriptEffect.actorValue);
    extra.bytesFrom(makeSubRecord(ESM::fourCC("EFIT"), efit.bytes));

    BytesBuilder scit;
    scit.u32(0x00001234); // script FormId
    scit.u32(Files::Oblivion::School_Destruction);
    scit.fourcc(ESM::fourCC("FIDG")); // visual effect
    scit.u32(Files::Oblivion::ScriptEffect_Hostile);
    extra.bytesFrom(makeSubRecord(ESM::fourCC("SCIT"), scit.bytes));

    extra.bytesFrom(makeStringSubRecord(ESM::fourCC("FULL"), "Custom Script Effect"));

    payload.insert(payload.end(), extra.bytes.begin(), extra.bytes.end());

    const std::filesystem::path path = makeTempPlugin(
        "spelscript.esp", buildPlugin({}, makeGroup(ESM::fourCC("SPEL"), Files::Oblivion::Grp_RecordType,
                                  makeRecord(Files::Oblivion::REC_SPEL, 0, 0x00000def, 0, payload))));

    Files::Oblivion::EsmFile plugin(path);
    plugin.parse();
    ASSERT_EQ(plugin.records().size(), 1u);

    const Files::Oblivion::Spell spell = Files::Oblivion::decodeSpell(plugin.records().front());
    EXPECT_EQ(spell.editorId, "FireBallScript");
    EXPECT_EQ(spell.name, "Fireball Script");
    EXPECT_EQ(spell.data.type, Files::Oblivion::Spell_Spell);
    EXPECT_EQ(spell.data.cost, 200u);
    EXPECT_EQ(spell.data.level, Files::Oblivion::Level_Expert);
    EXPECT_EQ(spell.data.flags, Files::Oblivion::Spell_AreaEffectIgnoresLOS);

    ASSERT_EQ(spell.effects.size(), 2u);

    EXPECT_EQ(spell.effects[0].effect.effectId, ESM::fourCC("FIDG"));
    EXPECT_EQ(spell.effects[0].effect.magnitude, 55u);
    EXPECT_EQ(spell.effects[0].effect.area, 3u);
    EXPECT_EQ(spell.effects[0].effect.duration, 7u);
    EXPECT_EQ(spell.effects[0].effect.deliveryType, Files::Oblivion::Delivery_Target);
    EXPECT_EQ(spell.effects[0].effect.actorValue, 42u);
    EXPECT_FALSE(spell.effects[0].script.has_value());

    EXPECT_EQ(spell.effects[1].effect.effectId, ESM::fourCC("SCPF"));
    ASSERT_TRUE(spell.effects[1].script.has_value());
    EXPECT_EQ(spell.effects[1].script->scriptFormId, 0x00001234u);
    EXPECT_EQ(spell.effects[1].script->school, Files::Oblivion::School_Destruction);
    EXPECT_EQ(spell.effects[1].script->visualEffectId, ESM::fourCC("FIDG"));
    EXPECT_EQ(spell.effects[1].script->flags, Files::Oblivion::ScriptEffect_Hostile);
    EXPECT_EQ(spell.effects[1].scriptName, "Custom Script Effect");
}

TEST(FilesOblivionEsm, decodesSpellWithShortScit)
{
    // Oblivion also writes SCIT subrecords shorter than 16 bytes; the missing fields default to 0
    std::vector<char> payload = makeSpellRecordPayload("ShortScript", "Short Script", Files::Oblivion::Spell_Spell,
        0, Files::Oblivion::Level_Novice, 0, {});

    {
        // Append a script effect whose SCIT only has room for the script FormId
        BytesBuilder efid;
        efid.fourcc(ESM::fourCC("SCPF"));
        std::vector<char> sub = makeSubRecord(ESM::fourCC("EFID"), efid.bytes);
        payload.insert(payload.end(), sub.begin(), sub.end());
    }
    {
        BytesBuilder efit;
        efit.fourcc(ESM::fourCC("SCPF"));
        efit.u32(0);
        efit.u32(0);
        efit.u32(0);
        efit.u32(Files::Oblivion::Delivery_Self);
        efit.u32(0);
        std::vector<char> sub = makeSubRecord(ESM::fourCC("EFIT"), efit.bytes);
        payload.insert(payload.end(), sub.begin(), sub.end());
    }
    {
        BytesBuilder scit; // only 4 bytes: the script FormId
        scit.u32(0x00000500);
        std::vector<char> sub = makeSubRecord(ESM::fourCC("SCIT"), scit.bytes);
        payload.insert(payload.end(), sub.begin(), sub.end());
    }

    const std::filesystem::path path = makeTempPlugin(
        "shortscit.esp", buildPlugin({}, makeGroup(ESM::fourCC("SPEL"), Files::Oblivion::Grp_RecordType,
                                makeRecord(Files::Oblivion::REC_SPEL, 0, 0x00000eee, 0, payload))));

    Files::Oblivion::EsmFile plugin(path);
    plugin.parse();
    ASSERT_EQ(plugin.records().size(), 1u);

    const Files::Oblivion::Spell spell = Files::Oblivion::decodeSpell(plugin.records().front());
    ASSERT_EQ(spell.effects.size(), 1u);
    ASSERT_TRUE(spell.effects[0].script.has_value());
    EXPECT_EQ(spell.effects[0].script->scriptFormId, 0x00000500u);
    EXPECT_EQ(spell.effects[0].script->school, 0u);
    EXPECT_EQ(spell.effects[0].script->visualEffectId, 0u);
    EXPECT_EQ(spell.effects[0].script->flags, 0u);
}

TEST(FilesOblivionEsm, decodesEnchantment)
{
    Files::Oblivion::EffectItem drainHealth{};
    drainHealth.magnitude = 25;
    drainHealth.area = 0;
    drainHealth.duration = 10;
    drainHealth.deliveryType = Files::Oblivion::Delivery_Touch;
    drainHealth.actorValue = 0;

    BytesBuilder payload;
    payload.bytesFrom(makeStringSubRecord(ESM::fourCC("EDID"), "TestEnchantment"));
    payload.bytesFrom(makeStringSubRecord(ESM::fourCC("FULL"), "Test Enchantment"));

    BytesBuilder enit;
    enit.u32(Files::Oblivion::Enchantment_Weapon);
    enit.u32(150); // charge amount
    enit.u32(30); // enchant cost
    enit.u32(Files::Oblivion::Enchantment_ManualCost);
    payload.bytesFrom(makeSubRecord(ESM::fourCC("ENIT"), enit.bytes));

    BytesBuilder efid;
    efid.fourcc(ESM::fourCC("FIDG"));
    payload.bytesFrom(makeSubRecord(ESM::fourCC("EFID"), efid.bytes));

    BytesBuilder efit;
    efit.fourcc(ESM::fourCC("FIDG"));
    efit.u32(drainHealth.magnitude);
    efit.u32(drainHealth.area);
    efit.u32(drainHealth.duration);
    efit.u32(drainHealth.deliveryType);
    efit.u32(drainHealth.actorValue);
    payload.bytesFrom(makeSubRecord(ESM::fourCC("EFIT"), efit.bytes));

    const std::filesystem::path path = makeTempPlugin(
        "ench.esp", buildPlugin({}, makeGroup(ESM::fourCC("ENCH"), Files::Oblivion::Grp_RecordType,
                             makeRecord(Files::Oblivion::REC_ENCH, 0, 0x00001234, 0, payload.bytes))));

    Files::Oblivion::EsmFile plugin(path);
    plugin.parse();
    ASSERT_EQ(plugin.records().size(), 1u);

    const Files::Oblivion::Enchantment enchantment = Files::Oblivion::decodeEnchantment(plugin.records().front());
    EXPECT_EQ(enchantment.editorId, "TestEnchantment");
    EXPECT_EQ(enchantment.name, "Test Enchantment");
    EXPECT_EQ(enchantment.data.type, Files::Oblivion::Enchantment_Weapon);
    EXPECT_EQ(enchantment.data.chargeAmount, 150u);
    EXPECT_EQ(enchantment.data.enchantCost, 30u);
    EXPECT_EQ(enchantment.data.flags, Files::Oblivion::Enchantment_ManualCost);

    ASSERT_EQ(enchantment.effects.size(), 1u);
    EXPECT_EQ(enchantment.effects[0].effect.effectId, ESM::fourCC("FIDG"));
    EXPECT_EQ(enchantment.effects[0].effect.magnitude, 25u);
    EXPECT_EQ(enchantment.effects[0].effect.deliveryType, Files::Oblivion::Delivery_Touch);
    EXPECT_TRUE(enchantment.effects[0].scriptName.empty());
}

TEST(FilesOblivionEsm, decodeSpellRequiresSpit)
{
    // A SPEL record without a SPIT is malformed
    std::vector<char> payload = makeStringSubRecord(ESM::fourCC("EDID"), "NoSpit");

    const std::filesystem::path path = makeTempPlugin(
        "nospit.esp", buildPlugin({}, makeGroup(ESM::fourCC("SPEL"), Files::Oblivion::Grp_RecordType,
                              makeRecord(Files::Oblivion::REC_SPEL, 0, 0x00000001, 0, payload))));

    Files::Oblivion::EsmFile plugin(path);
    plugin.parse();
    ASSERT_EQ(plugin.records().size(), 1u);

    EXPECT_THROW(Files::Oblivion::decodeSpell(plugin.records().front()), std::runtime_error);
}

TEST(FilesOblivionEsm, handlesExtendedSubRecordSizes)
{
    PluginOptions options;

    // A subrecord larger than 0xffff bytes must be prefixed with an XXXX subrecord
    std::vector<char> bigData(0x10000, 'A');

    BytesBuilder payload;
    payload.bytesFrom(makeStringSubRecord(ESM::fourCC("EDID"), "BigRecord"));

    BytesBuilder xxxx;
    xxxx.u32(static_cast<std::uint32_t>(bigData.size()));
    payload.bytesFrom(makeSubRecord(ESM::fourCC("XXXX"), xxxx.bytes));

    BytesBuilder bigSub;
    bigSub.fourcc(ESM::fourCC("DATA"));
    bigSub.u16(0); // dataSize is overridden by the XXXX subrecord
    bigSub.bytesFrom(bigData);
    payload.bytesFrom(bigSub.bytes);

    const std::filesystem::path path = makeTempPlugin(
        "xxxx.esp", buildPlugin({}, makeGroup(ESM::fourCC("STAT"), Files::Oblivion::Grp_RecordType,
                             makeRecord(Files::Oblivion::REC_STAT, 0, 0x00000055, 0, payload.bytes))));

    Files::Oblivion::EsmFile plugin(path);
    plugin.parse();

    ASSERT_EQ(plugin.records().size(), 1u);
    const Files::Oblivion::Record& record = plugin.records().front();
    ASSERT_EQ(record.subRecords.size(), 2u);
    ASSERT_EQ(record.subRecords[1].type, ESM::fourCC("DATA"));
    EXPECT_EQ(record.subRecords[1].data.size(), bigData.size());
}

TEST(FilesOblivionEsm, findSubRecordHelpers)
{
    PluginOptions options;

    const std::vector<char> spellPayload = makeSpellRecordPayload("FindSub", "Find Sub",
        Files::Oblivion::Spell_Power, 10, Files::Oblivion::Level_Apprentice, 0, {});

    const std::filesystem::path path = makeTempPlugin(
        "findsub.esp", buildPlugin({}, makeGroup(ESM::fourCC("SPEL"), Files::Oblivion::Grp_RecordType,
                              makeRecord(Files::Oblivion::REC_SPEL, 0, 0x00000077, 0, spellPayload))));

    Files::Oblivion::EsmFile plugin(path);
    plugin.parse();
    ASSERT_EQ(plugin.records().size(), 1u);
    const Files::Oblivion::Record& record = plugin.records().front();

    const Files::Oblivion::SubRecord* edid = record.findSubRecord(ESM::fourCC("EDID"));
    ASSERT_NE(edid, nullptr);
    EXPECT_EQ(edid->asZString(), "FindSub");

    EXPECT_EQ(record.findSubRecord(ESM::fourCC("WTHR")), nullptr);

    const std::vector<const Files::Oblivion::SubRecord*> fulls = record.findSubRecords(ESM::fourCC("FULL"));
    ASSERT_EQ(fulls.size(), 1u);
    EXPECT_EQ(fulls.front()->asZString(), "Find Sub");
}
