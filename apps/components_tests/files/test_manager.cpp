// Oblivion (TES4) file format support tests.
//
// Note: this repository has no top-level "tests/components/files" directory; the
// component test suite lives in apps/components_tests, so this file is registered
// there (see CMakeLists.txt next to this file).

#include <components/esm/gameformat.hpp>

#include <components/bsa/bsafile.hpp>
#include <components/bsa/compressedbsafile.hpp>
#include <components/nif/niffile.hpp>

#include <components/testing/util.hpp>
#include <components/vfs/bsaarchive.hpp>
#include <components/vfs/filemap.hpp>

#include <gtest/gtest.h>

#include <cstdint>
#include <filesystem>
#include <fstream>
#include <istream>
#include <iterator>
#include <optional>
#include <string>
#include <vector>

namespace
{
    void writeHeader(const std::filesystem::path& path, std::uint32_t magic, std::uint32_t version)
    {
        std::ofstream stream(path, std::ios::binary);
        stream.exceptions(std::ofstream::failbit | std::ofstream::badbit);

        const std::uint32_t head[3] = { magic, version, 36 };
        stream.write(reinterpret_cast<const char*>(head), sizeof(head));
    }

    // Replicates CompressedBSAFile::generateHash (private) so the synthetic archive
    // contains hashes the loader can find again. Keep in sync with compressedbsafile.cpp.
    std::uint64_t generateOblivionBsaHash(std::string_view str, std::string_view extension)
    {
        if (str.empty())
            return 0;

        const auto at = [&](std::size_t i) -> unsigned char {
            const char c = str[i];
            return c == '/' ? '\\' : c;
        };

        const std::size_t len = str.length();

        std::uint64_t result = at(len - 1);
        if (len >= 3)
            result |= static_cast<std::uint64_t>(at(len - 2)) << 8;
        result |= static_cast<std::uint64_t>(len) << 16;
        result |= static_cast<std::uint64_t>(at(0)) << 24;
        if (len >= 4)
        {
            std::uint32_t hash = 0;
            for (std::size_t i = 1; i <= len - 3; ++i)
                hash = hash * 0x1003f + at(i);
            result += static_cast<std::uint64_t>(hash) << 32;
        }

        if (extension.empty())
            return result;

        if (extension == ".kf")
            result |= 0x80;
        else if (extension == ".nif")
            result |= 0x8000;
        else if (extension == ".dds")
            result |= 0x8080;
        else if (extension == ".wav")
            result |= 0x80000000;

        std::uint32_t hash = 0;
        for (unsigned char c : extension)
            hash = hash * 0x1003f + c;
        result += static_cast<std::uint64_t>(hash) << 32;

        return result;
    }

    // Write a minimal Oblivion (version 103 / 0x67) BSA archive containing a single file
    // "folder\filename" with the given payload.
    void writeOblivionBsa(const std::filesystem::path& path, const std::string& payload)
    {
        constexpr std::uint32_t FolderNamesFlag = Bsa::CompressedBSAFile::ArchiveFlag_FolderNames;
        constexpr std::uint32_t FileNamesFlag = Bsa::CompressedBSAFile::ArchiveFlag_FileNames;

        const std::string folderName = "folder";
        const std::string fileName = "filename";
        constexpr std::uint32_t headerSize = sizeof(Bsa::CompressedBSAFile::Header);
        static_assert(headerSize == 36);

        const std::uint64_t folderHash = generateOblivionBsaHash(folderName, {});
        const std::uint64_t fileHash = generateOblivionBsaHash(fileName, {});

        constexpr std::uint32_t folderRecordSize = 8 + 4 + 4; // hash + count + offset
        constexpr std::uint32_t folderNameBlockSize = 1 + 7; // length byte + "folder\0"
        constexpr std::uint32_t fileRecordSize = 8 + 4 + 4; // hash + size + offset
        constexpr std::uint32_t fileNameBlockSize = 9; // "filename\0"

        const std::uint32_t foldersOffset = headerSize;
        const std::uint32_t fileRecordOffset = foldersOffset + folderRecordSize + folderNameBlockSize;
        const std::uint32_t fileNameOffset = fileRecordOffset + fileRecordSize;
        const std::uint32_t dataOffset = fileNameOffset + fileNameBlockSize;

        std::ofstream stream(path, std::ios::binary);
        stream.exceptions(std::ofstream::failbit | std::ofstream::badbit);

        const Bsa::CompressedBSAFile::Header header{
            .mFormat = static_cast<std::uint32_t>(Bsa::BsaVersion::Compressed), // "BSA\0"
            .mVersion = Bsa::CompressedBSAFile::Version_TES4, // 0x67 = 103, Oblivion
            .mFoldersOffset = foldersOffset,
            .mFlags = FolderNamesFlag | FileNamesFlag,
            .mFolderCount = 1,
            .mFileCount = 1,
            .mFolderNamesLength = 7, // "folder\0"
            .mFileNamesLength = 9, // "filename\0"
            .mFileFlags = 0,
        };
        stream.write(reinterpret_cast<const char*>(&header), sizeof(header));

        stream.write(reinterpret_cast<const char*>(&folderHash), sizeof(folderHash));
        const std::uint32_t folderFileCount = 1;
        stream.write(reinterpret_cast<const char*>(&folderFileCount), sizeof(folderFileCount));
        const std::int32_t folderOffset = static_cast<std::int32_t>(fileRecordOffset); // ignored by the loader
        stream.write(reinterpret_cast<const char*>(&folderOffset), sizeof(folderOffset));

        const std::uint8_t folderNameSize = static_cast<std::uint8_t>(folderName.size() + 1);
        stream.write(reinterpret_cast<const char*>(&folderNameSize), sizeof(folderNameSize));
        stream.write(folderName.data(), folderName.size());
        stream.put('\0');

        stream.write(reinterpret_cast<const char*>(&fileHash), sizeof(fileHash));
        const std::uint32_t fileSize = static_cast<std::uint32_t>(payload.size());
        stream.write(reinterpret_cast<const char*>(&fileSize), sizeof(fileSize));
        const std::uint32_t singleFileOffset = dataOffset;
        stream.write(reinterpret_cast<const char*>(&singleFileOffset), sizeof(singleFileOffset));

        stream.write(fileName.data(), fileName.size());
        stream.put('\0');

        stream.write(payload.data(), payload.size());
    }

    std::string readStreamContents(std::istream& stream)
    {
        return std::string(std::istreambuf_iterator<char>(stream), std::istreambuf_iterator<char>());
    }
}

TEST(OblivionFileFormat, test_oblivion_file_format_detection)
{
    using ESM::GameFormat;

    // Morrowind (TES3) plugin versions 1.2/1.3 are detected as Morrowind.
    EXPECT_EQ(ESM::getGameFormat(ESM::VER_120, std::nullopt), GameFormat::TES3);
    EXPECT_EQ(ESM::getGameFormat(ESM::VER_130, std::nullopt), GameFormat::TES3);
    EXPECT_TRUE(ESM::isMorrowindVersion(ESM::VER_120));
    EXPECT_TRUE(ESM::isMorrowindVersion(ESM::VER_130));
    EXPECT_FALSE(ESM::isMorrowindVersion(ESM::VER_080));

    // Oblivion (TES4) plugin version 0.8 is detected as Oblivion.
    EXPECT_EQ(ESM::getGameFormat(ESM::VER_080, std::nullopt), GameFormat::TES4);
    EXPECT_TRUE(ESM::isOblivionVersion(ESM::VER_080));
    EXPECT_FALSE(ESM::isOblivionVersion(ESM::VER_120));
    EXPECT_FALSE(ESM::isOblivionVersion(ESM::VER_130));

    // VER_100 is shared between TES4 and FO4: with no header form version it maps to TES4,
    // with any header form version it maps to FO4.
    EXPECT_EQ(ESM::getGameFormat(ESM::VER_100, std::nullopt), GameFormat::TES4);
    EXPECT_EQ(ESM::getGameFormat(ESM::VER_100, 0), GameFormat::FO4);
    EXPECT_EQ(ESM::getGameFormat(ESM::VER_100, 131), GameFormat::FO4);

    const std::filesystem::path oblivionBsa = TestingOpenMW::outputFilePath("oblivion.bsa");
    writeHeader(oblivionBsa, static_cast<std::uint32_t>(Bsa::BsaVersion::Compressed), 0x67); // version 103
    EXPECT_EQ(Bsa::BSAFile::detectVersion(oblivionBsa), Bsa::BsaVersion::TES4);

    const std::filesystem::path morrowindBsa = TestingOpenMW::outputFilePath("morrowind.bsa");
    writeHeader(morrowindBsa, static_cast<std::uint32_t>(Bsa::BsaVersion::Uncompressed), 0x0);
    EXPECT_EQ(Bsa::BSAFile::detectVersion(morrowindBsa), Bsa::BsaVersion::Uncompressed);

    const std::filesystem::path fallout3Bsa = TestingOpenMW::outputFilePath("fallout3.bsa");
    writeHeader(fallout3Bsa, static_cast<std::uint32_t>(Bsa::BsaVersion::Compressed), 0x68);
    EXPECT_EQ(Bsa::BSAFile::detectVersion(fallout3Bsa), Bsa::BsaVersion::FO3);

    const std::filesystem::path unknownBsa = TestingOpenMW::outputFilePath("unknown.bsa");
    writeHeader(unknownBsa, 0xDEADBEEF, 0x0);
    EXPECT_EQ(Bsa::BSAFile::detectVersion(unknownBsa), Bsa::BsaVersion::Unknown);
}

TEST(OblivionFileFormat, test_oblivion_bsa_loading)
{
    const std::filesystem::path path = TestingOpenMW::outputFilePath("test_oblivion_bsa_loading.bsa");

    std::string payload;
    while (payload.size() < 42)
        payload += "Oblivion!";
    payload.resize(42);

    writeOblivionBsa(path, payload);

    // Detected as an Oblivion (TES4, version 103) archive, not a generic compressed BSA.
    ASSERT_EQ(Bsa::BSAFile::detectVersion(path), Bsa::BsaVersion::TES4);

    Bsa::CompressedBSAFile file;
    file.open(path);

    ASSERT_EQ(file.getList().size(), 1);
    EXPECT_EQ(std::string(file.getList().front().name()), "folder\\filename");
    EXPECT_EQ(file.getList().front().mFileSize, payload.size());

    Files::IStreamPtr stream = file.getFile(&file.getList().front());
    ASSERT_TRUE(stream);
    EXPECT_EQ(readStreamContents(*stream), payload);

    // The VFS archive factory must be able to load Oblivion BSAs too.
    std::unique_ptr<VFS::Archive> archive = VFS::makeBsaArchive(path, nullptr);
    ASSERT_NE(archive, nullptr);

    VFS::FileMap resources;
    archive->listResources(resources);

    const auto it = resources.find(VFS::Path::Normalized("folder/filename"));
    ASSERT_NE(it, resources.end());

    Files::IStreamPtr archiveStream = it->second->open();
    ASSERT_TRUE(archiveStream);
    EXPECT_EQ(readStreamContents(*archiveStream), payload);
}

TEST(OblivionFileFormat, test_oblivion_nif_support)
{
    using Nif::NIFFile;

    // Main Oblivion NIF structure versions (20.0.0.5 and the older 10.0.1.2) are recognized.
    EXPECT_TRUE(Nif::isOblivionNIFVersion(NIFFile::VER_OB));
    EXPECT_TRUE(Nif::isOblivionNIFVersion(NIFFile::VER_OB_OLD));

    EXPECT_EQ(static_cast<std::uint32_t>(NIFFile::VER_OB), 0x14000005);
    EXPECT_EQ(static_cast<std::uint32_t>(NIFFile::VER_OB_OLD), 0x0A000102);

    // Morrowind's NIF (4.0.0.2) is not mistaken for an Oblivion NIF.
    EXPECT_FALSE(Nif::isOblivionNIFVersion(NIFFile::VER_MW));
    EXPECT_TRUE(Nif::isMorrowindNIFVersion(NIFFile::VER_MW));
    EXPECT_FALSE(Nif::isMorrowindNIFVersion(NIFFile::VER_OB));

    // The Gamebryo NIF version used by post-Oblivion titles is not an Oblivion NIF either.
    EXPECT_FALSE(Nif::isOblivionNIFVersion(NIFFile::VER_BGS));
    EXPECT_TRUE(Nif::isGamebryoNIFVersion(NIFFile::VER_BGS));
}
