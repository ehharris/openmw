// Tests for bridging TES4 (ESM4) GLOB records into the esm3 ESM::Global representation.
//
// The converter lives in components/esm4/convertglob.{hpp,cpp} and is part of libcomponents, so this test
// can exercise it directly. MWWorld::Store<ESM::Global> itself lives in the `openmw` application target and
// cannot be linked from components-tests; we reproduce its exact storage (std::unordered_map<ESM::RefId,
// ESM::Global>, which is TypedDynamicStore<ESM::Global>::Static) to pin the case-insensitive lookup that the
// engine relies on when DateTimeManager looks up "gamehour".

#include <components/esm3/loadglob.hpp>
#include <components/esm3/variant.hpp>
#include <components/esm/refid.hpp>
#include <components/esm4/convertglob.hpp>
#include <components/esm4/loadglob.hpp>

#include <gtest/gtest.h>

#include <cstdint>
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
