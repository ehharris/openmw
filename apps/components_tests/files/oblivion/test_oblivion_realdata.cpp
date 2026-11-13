// Integration tests for the Oblivion components against the user's REAL game data.
//
// These tests validate the ESM parser (components/files/oblivion/esm.{hpp,cpp}), the
// content validation (components/oblivion/contentloader.{hpp,cpp}) and the BSA reader
// (components/files/oblivion/bsa.{hpp,cpp}) against an actual Oblivion installation.
//
// The game data location is provided through the OPENMW_TEST_OBLIVION_DATA environment
// variable, pointing at the game's Data directory (the one containing Oblivion.esm and
// the "Oblivion - *.bsa" archives). When the variable is unset or does not point to an
// existing directory every test SKIPS cleanly, so CI without game data stays green.
//
// The game data is treated as strictly READ-ONLY: these tests only ever open files for
// reading and never write anything into it.

#include <components/files/oblivion/bsa.hpp>
#include <components/files/oblivion/esm.hpp>
#include <components/oblivion/contentloader.hpp>

#include <gtest/gtest.h>

#include <algorithm>
#include <cctype>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <istream>
#include <iterator>
#include <string>
#include <vector>

namespace
{
    // Base class for the real-data suite: resolves OPENMW_TEST_OBLIVION_DATA and skips
    // (rather than fails) when it is unset or points nowhere.
    class OblivionRealData : public ::testing::Test
    {
    protected:
        std::filesystem::path mDataDir;

        void SetUp() override
        {
            const char* env = std::getenv("OPENMW_TEST_OBLIVION_DATA");
            if (env == nullptr || *env == '\0')
                GTEST_SKIP() << "OPENMW_TEST_OBLIVION_DATA is not set";

            mDataDir = std::filesystem::path(env);
            std::error_code ec;
            if (!std::filesystem::is_directory(mDataDir, ec))
                GTEST_SKIP() << "OPENMW_TEST_OBLIVION_DATA=" << env << " is not an existing directory";
        }

        // Returns true when the given file exists below the data directory. Otherwise
        // records a skip for the current test and returns false - GTEST_SKIP() expands
        // to `return <void>` and cannot be used in a non-void helper, so the skip is
        // recorded with the underlying GTEST_MESSAGE_ macro instead; every caller must
        // return immediately on false (e.g. `if (!requireFile(x)) return;`).
        bool requireFile(const std::filesystem::path& relative)
        {
            if (std::filesystem::is_regular_file(mDataDir / relative))
                return true;
            const std::string message = (mDataDir / relative).string() + " does not exist";
            GTEST_MESSAGE_(message.c_str(), ::testing::TestPartResult::kSkip);            return false;
        }
    };

    // Reads the whole stream into a byte vector.
    std::vector<char> readAll(std::istream& stream)
    {
        return std::vector<char>(std::istreambuf_iterator<char>(stream), std::istreambuf_iterator<char>());
    }

    bool startsWith(const std::vector<char>& data, std::string_view prefix)
    {
        if (data.size() < prefix.size())
            return false;
        return std::equal(prefix.begin(), prefix.end(), data.begin());
    }

    // Extensions whose extracted contents we can sanity-check. Note that the official
    // archives do not only contain DDS/NIF/WAV/OGG: Voices1 stores MP3 voicing, and
    // Misc has no media files at all (the first addressable entry there is XML).
    bool isKnownExtension(const std::string& extension)
    {
        return extension == ".dds" || extension == ".nif" || extension == ".wav" || extension == ".ogg"
            || extension == ".mp3" || extension == ".xml";
    }

    // The expected file signature ("magic bytes") for the known extensions.
    bool hasPlausibleMagic(const std::string& extension, const std::vector<char>& data)
    {
        if (extension == ".dds")
            return startsWith(data, "DDS ");
        if (extension == ".nif")
            // Gamebryo-era files (what Oblivion ships) start with "Gamebryo File",
            // the older NetImmerse ones with "NETFRAME"; accept both.
            return startsWith(data, "Gamebryo") || startsWith(data, "NETFRAME");
        if (extension == ".wav")
            return startsWith(data, "RIFF");
        if (extension == ".ogg")
            return startsWith(data, "OggS");
        if (extension == ".mp3")
            return startsWith(data, "ID3") || (data.size() >= 2 && static_cast<unsigned char>(data[0]) == 0xFF
                && (static_cast<unsigned char>(data[1]) & 0xE0) == 0xE0); // MPEG frame sync
        if (extension == ".xml")
            // '<' followed by a tag name, an XML declaration or a comment/CDATA marker.
            return data.size() >= 2 && data[0] == '<'
                && (data[1] == '?' || data[1] == '!' || std::isalpha(static_cast<unsigned char>(data[1])));
        return false;
    }

    std::string lowerCase(std::string s)
    {
        std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) {
            return static_cast<char>(std::tolower(c));
        });
        return s;
    }
}

TEST_F(OblivionRealData, ParsesVanillaOblivionEsm)
{
    if (!requireFile("Oblivion.esm"))
        return;

    Files::Oblivion::EsmFile esm;
    ASSERT_NO_THROW(esm.open(mDataDir / "Oblivion.esm"));
    ASSERT_TRUE(esm.isOpen());

    // Vanilla Oblivion.esm is a master with HEDR version 1.0.
    const auto& header = esm.header();
    EXPECT_FLOAT_EQ(header.mData.version.f, 1.0f);
    EXPECT_TRUE(Files::Oblivion::isOblivionHedrVersion(header.mData.version.ui));
    EXPECT_TRUE(header.isMaster());

    // Materialise the whole file (this is the expensive step: vanilla Oblivion.esm
    // holds well over a million records and groups).
    ASSERT_NO_THROW(esm.parse());
    EXPECT_GT(esm.records().size(), 1'000'000u);

    // Well-known vanilla records must be findable by type + EDID.
    const auto& records = esm.records();
    bool foundGameHour = false;
    bool foundSkillBlade = false;
    for (const auto& record : records)
    {
        if (record.typeId != Files::Oblivion::REC_GLOB && record.typeId != Files::Oblivion::REC_SKIL)
            continue;
        const auto* edid = record.findSubRecord(ESM::fourCC("EDID"));
        if (edid == nullptr)
            continue;
        if (record.typeId == Files::Oblivion::REC_GLOB && edid->asZString() == "GameHour")
            foundGameHour = true;
        else if (record.typeId == Files::Oblivion::REC_SKIL && edid->asZString() == "SkillBlade")
            foundSkillBlade = true;
        if (foundGameHour && foundSkillBlade)
            break;
    }
    EXPECT_TRUE(foundGameHour) << "GLOB with EDID 'GameHour' not found in Oblivion.esm";
    EXPECT_TRUE(foundSkillBlade) << "SKIL with EDID 'SkillBlade' not found in Oblivion.esm";
}

TEST_F(OblivionRealData, ValidateVanillaMasterNoErrors)
{
    if (!requireFile("Oblivion.esm"))
        return;

    // validate() must accept the vanilla master. Before the whitespace-only SNAM/MAST
    // filtering fix this threw on real data due to the vanilla ' ' quirk.
    Files::Oblivion::Header header;
    ASSERT_NO_THROW(header = Oblivion::ContentLoader::validate(mDataDir / "Oblivion.esm"));

    // Vanilla Oblivion.esm has no master dependencies, so nothing can be missing from it.
    EXPECT_TRUE(header.mMaster.empty());
}

TEST_F(OblivionRealData, ReadsAllOfficialBSAs)
{
    // The five core archives shipped by Oblivion (voicing is split across Voices1/2;
    // the brief pins Voices1).
    const char* archiveNames[] = {
        "Oblivion - Meshes.bsa",
        "Oblivion - Textures - Compressed.bsa",
        "Oblivion - Sounds.bsa",
        "Oblivion - Voices1.bsa",
        "Oblivion - Misc.bsa",
    };

    for (const char* name : archiveNames)
    {
        SCOPED_TRACE(name);
        if (!requireFile(name))
            return;

        Files::Oblivion::BsaFile bsa;
        ASSERT_NO_THROW(bsa.open(mDataDir / name));
        ASSERT_TRUE(bsa.isOpen());

        // All official Oblivion archives are version 103.
        EXPECT_EQ(bsa.header().mVersion, Files::Oblivion::BsaFile::ArchiveVersion);
        EXPECT_EQ(bsa.header().mVersion, 103u);

        // They all store file names and a non-empty file table.
        EXPECT_TRUE(bsa.header().hasFileNames());
        EXPECT_GT(bsa.header().mFileCount, 0u);
        EXPECT_FALSE(bsa.getList().empty());

        // Extract the first entry with an extension we can sanity-check the contents of.
        const Bsa::BSAFile::FileStruct* chosen = nullptr;
        std::string chosenExt;
        for (const auto& file : bsa.getList())
        {
            const std::string lowered = lowerCase(std::string(file.name()));
            const std::size_t dotPos = lowered.find_last_of('.');
            if (dotPos == std::string::npos)
                continue;
            const std::string extension = lowered.substr(dotPos);
            if (!isKnownExtension(extension))
                continue;
            chosen = &file;
            chosenExt = extension;
            break;
        }
        ASSERT_NE(chosen, nullptr) << "no extractable entry with a known extension in " << name;

        const std::string chosenName = lowerCase(std::string(chosen->name()));
        Files::IStreamPtr stream;
        ASSERT_NO_THROW(stream = bsa.getFile(chosen));
        ASSERT_NE(stream, nullptr);

        const std::vector<char> data = readAll(*stream);
        EXPECT_FALSE(data.empty()) << chosenName;
        EXPECT_TRUE(hasPlausibleMagic(chosenExt, data)) << "unexpected magic bytes for " << chosenName;
    }
}
