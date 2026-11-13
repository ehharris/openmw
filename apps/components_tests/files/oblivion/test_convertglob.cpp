// Tests for bridging TES4 (ESM4) GLOB records into the esm3 ESM::Global representation.
//
// The converter lives in components/esm4/convertglob.{hpp,cpp} and is part of libcomponents, so this test
// can exercise it directly. MWWorld::Store<ESM::Global> itself lives in the `openmw` application target and
// cannot be linked from components-tests; we reproduce its exact storage (std::unordered_map<ESM::RefId,
// ESM::Global>, which is TypedDynamicStore<ESM::Global>::Static) to pin the case-insensitive lookup that the
// engine relies on when DateTimeManager looks up "gamehour".
//
// Some cases feed raw FLTV bytes through the real ESM4 streaming loader (synthetic TES4 file in memory +
// ESM4::Reader/ReaderUtils, the same path apps/openmw uses) to pin the float-bit FLTV convention end to end.

#include <components/esm/common.hpp>
#include <components/esm/fourcc.hpp>
#include <components/esm3/loadglob.hpp>
#include <components/esm3/variant.hpp>
#include <components/esm/refid.hpp>
#include <components/esm4/convertglob.hpp>
#include <components/esm4/loadglob.hpp>
#include <components/esm4/reader.hpp>
#include <components/esm4/readerutils.hpp>

#include <gtest/gtest.h>

#include <cstdint>
#include <memory>
#include <sstream>
#include <string>
#include <string_view>
#include <unordered_map>

namespace
{
    ESM4::GlobalVariable makeGlob(std::string_view editorId, std::uint8_t type, float value)
    {
        ESM4::GlobalVariable glob;
        glob.mEditorId = std::string(editorId);
        glob.mType = type;
        glob.mValue = value;
        return glob;
    }

    // Little-endian byte sink for building a synthetic TES4 file in memory.
    struct ByteBuilder
    {
        std::string bytes;

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
        void raw(std::string_view data) { bytes.append(data); }
    };

    std::string makeSubRecord(std::uint32_t type, std::string_view payload)
    {
        ByteBuilder builder;
        builder.u32(type);
        builder.u16(static_cast<std::uint16_t>(payload.size()));
        builder.raw(payload);
        return std::move(builder.bytes);
    }

    // TES4-family record header: type, dataSize, flags, formId, revision (20 bytes; no version field).
    std::string makeRecord(std::uint32_t type, std::string_view payload)
    {
        ByteBuilder builder;
        builder.u32(type);
        builder.u32(static_cast<std::uint32_t>(payload.size()));
        builder.u32(0); // flags
        builder.u32(0x00000014); // formId
        builder.u32(0); // version control info / revision
        builder.raw(payload);
        return std::move(builder.bytes);
    }

    // Builds a minimal TES4 file containing exactly one GLOB record with the given FNAM type letter and the
    // exact FLTV bytes given (raw 32-bit pattern, no interpretation by the builder).
    std::string buildTes4WithGlob(std::string_view editorId, char fnamType, std::uint32_t fltvRawBits)
    {
        ByteBuilder hedr;
        hedr.u32(ESM::VER_100); // float bits are just the raw 32-bit pattern (vanilla Oblivion header version)
        hedr.u32(0); // record count
        hedr.u32(0x00000014); // next object id

        ByteBuilder tes4Payload;
        tes4Payload.raw(makeSubRecord(ESM::fourCC("HEDR"), hedr.bytes));

        ByteBuilder globPayload;
        globPayload.raw(makeSubRecord(ESM::fourCC("EDID"), std::string(editorId) + '\0'));
        globPayload.raw(makeSubRecord(ESM::fourCC("FNAM"), std::string_view(&fnamType, 1)));
        ByteBuilder fltv;
        fltv.u32(fltvRawBits);
        globPayload.raw(makeSubRecord(ESM::fourCC("FLTV"), fltv.bytes));

        return makeRecord(ESM::fourCC("TES4"), tes4Payload.bytes) + makeRecord(ESM::fourCC("GLOB"), globPayload.bytes);
    }

    struct LoadedGlob
    {
        ESM4::GlobalVariable glob;
        std::size_t count = 0;
    };

    // Feeds the synthetic file through the real streaming path: an ESM4::Reader (which parses the TES4 header)
    // iterated with ReaderUtils::readAll, loading each GLOB with ESM4::GlobalVariable::load — exactly what the
    // engine's ESMStore ESM4 visitor does.
    LoadedGlob loadGlobs(const std::string& fileBytes)
    {
        auto stream = std::make_unique<std::istringstream>(fileBytes, std::ios_base::in | std::ios_base::binary);
        ESM4::Reader reader(std::move(stream), "synthetic.esm", /*vfs=*/nullptr, /*encoder=*/nullptr);

        LoadedGlob result;
        ESM4::ReaderUtils::readAll(
            reader,
            [&result](ESM4::Reader& recordReader) {
                if (recordReader.hdr().record.typeId != ESM::fourCC("GLOB"))
                    return false;
                recordReader.getRecordData();
                result.glob.load(recordReader);
                ++result.count;
                return true;
            },
            [](ESM4::Reader&) {});
        return result;
    }
}

TEST(OblivionConvertGlob, convertsFloatAndPinsCaseInsensitiveLookup)
{
    const ESM4::GlobalVariable glob = makeGlob("GameHour", 'f', 12.5f);

    const auto converted = ESM4::convertToESM3(glob);
    ASSERT_TRUE(converted.has_value());

    // The converted global carries the float type and value.
    EXPECT_EQ(converted->mValue.getType(), ESM::VT_Float);
    EXPECT_FLOAT_EQ(converted->mValue.getFloat(), 12.5f);
    EXPECT_TRUE(converted->mId == ESM::RefId::stringRefId("GameHour"));

    // Pin CI lookup using the exact container MWWorld::Store<ESM::Global> uses internally:
    // std::unordered_map<ESM::RefId, ESM::Global>. A lower-case EDID query must resolve the mixed-case record.
    std::unordered_map<ESM::RefId, ESM::Global> store;
    store.emplace(converted->mId, *converted);

    const auto it = store.find(ESM::RefId::stringRefId("gamehour"));
    ASSERT_NE(it, store.end());
    EXPECT_FLOAT_EQ(it->second.mValue.getFloat(), 12.5f);
}

TEST(OblivionConvertGlob, mapsShortTypeToVariantShort)
{
    const ESM4::GlobalVariable glob = makeGlob("TestShort", 's', 7.f);

    const auto converted = ESM4::convertToESM3(glob);
    ASSERT_TRUE(converted.has_value());

    EXPECT_EQ(converted->mValue.getType(), ESM::VT_Short);
    EXPECT_EQ(converted->mValue.getInteger(), 7);
}

TEST(OblivionConvertGlob, mapsLongTypeToVariantLong)
{
    const ESM4::GlobalVariable glob = makeGlob("TestLong", 'l', 42.f);

    const auto converted = ESM4::convertToESM3(glob);
    ASSERT_TRUE(converted.has_value());

    EXPECT_EQ(converted->mValue.getType(), ESM::VT_Long);
    EXPECT_EQ(converted->mValue.getInteger(), 42);
}

TEST(OblivionConvertGlob, returnsNulloptForUnknownType)
{
    const ESM4::GlobalVariable glob = makeGlob("BadType", 'x', 1.f);

    EXPECT_FALSE(ESM4::convertToESM3(glob).has_value());
}

TEST(OblivionConvertGlob, returnsNulloptForEmptyEditorId)
{
    // An EDID-less GLOB must not be inserted into the store under an empty RefId.
    const ESM4::GlobalVariable glob = makeGlob("", 'f', 1.f);

    EXPECT_FALSE(ESM4::convertToESM3(glob).has_value());
}

TEST(OblivionConvertGlob, byteLevelFloatBitsFltvBridgesThroughTheRealLoader)
{
    // Oblivion's convention: integer-typed globals store the value as a float in FLTV. Feed the raw bit
    // pattern of 7.0f (0x40E00000) through ESM4::GlobalVariable::load and pin that an 's'-typed glob with
    // this on-disk representation bridges to the integer 7 (the static_cast in convertToESM3 is correct for
    // Oblivion files because of this convention).
    const LoadedGlob loaded = loadGlobs(buildTes4WithGlob("TestShort", 's', 0x40E00000u));
    ASSERT_EQ(loaded.count, 1u);
    EXPECT_EQ(loaded.glob.mEditorId, "TestShort");
    EXPECT_FLOAT_EQ(loaded.glob.mValue, 7.0f);

    const auto converted = ESM4::convertToESM3(loaded.glob);
    ASSERT_TRUE(converted.has_value());
    EXPECT_EQ(converted->mValue.getType(), ESM::VT_Short);
    EXPECT_EQ(converted->mValue.getInteger(), 7);
}

TEST(OblivionConvertGlob, byteLevelIntegerEncodedFltvBridgesToZeroKnownLimitation)
{
    // Counterpart of the float-bits test: a hypothetical TES4-family file storing true integer-encoded FLTV
    // bytes (42 as 0x0000002A). The reader always interprets FLTV as a float, so this reads as a denormal
    // ~3.9e-43 and bridges to 0 — the documented limitation in convertglob.hpp. No Oblivion file does this;
    // this case pins that behaviour so a change to it is deliberate.
    const LoadedGlob loaded = loadGlobs(buildTes4WithGlob("TestShort", 's', 0x0000002Au));
    ASSERT_EQ(loaded.count, 1u);

    const auto converted = ESM4::convertToESM3(loaded.glob);
    ASSERT_TRUE(converted.has_value());
    EXPECT_EQ(converted->mValue.getType(), ESM::VT_Short);
    EXPECT_EQ(converted->mValue.getInteger(), 0);
}
