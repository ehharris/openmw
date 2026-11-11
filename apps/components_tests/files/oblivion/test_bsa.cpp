// Oblivion (TES4, version 103) BSA archive reader tests.
//
// These tests build synthetic Oblivion BSA archives in a temporary directory and read
// them with components/files/oblivion/bsa.{hpp,cpp}. The component test suite lives in
// apps/components_tests, so this file is registered there (see CMakeLists.txt next to
// this file's parent directory).

#include <components/files/oblivion/bsa.hpp>

#include <components/testing/util.hpp>

#include <gtest/gtest.h>

#include <zlib.h>

#include <algorithm>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <map>
#include <string>
#include <vector>

namespace
{
    // Replicates Files::Oblivion::BsaFile::generateHash for the test archives, so the
    // synthetic files can be found again by the reader. Kept in sync with bsa.cpp.
    std::uint64_t testGenerateHash(std::string_view str, std::string_view extension)
    {
        if (str.empty())
            return 0;
        const auto at = [&](std::size_t i) -> unsigned char {
            const char c = str[i];
            return c == '/' ? '\\' : static_cast<unsigned char>(c);
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
        for (const unsigned char c : extension)
            hash = hash * 0x1003f + c;
        result += static_cast<std::uint64_t>(hash) << 32;
        return result;
    }

    struct TestFile
    {
        std::string folder; // stored with backslashes; "" = archive root (hash 0)
        std::string name; // bare file name including extension
        std::string data;
        bool compressed = false; // store this file's bytes zlib compressed
    };

    struct TestArchiveSpec
    {
        std::vector<TestFile> files;
        std::uint32_t flags = Files::Oblivion::ArchiveFlag_FolderNames | Files::Oblivion::ArchiveFlag_FileNames;
        std::uint32_t version = Files::Oblivion::BsaFile::ArchiveVersion; // 0x67
        std::uint32_t magic = Files::Oblivion::BsaFile::ArchiveMagic; // "BSA\0"
        std::uint32_t fileFlags = 0x0100; // FileFlag_Misc, as in basically every official archive
    };

    std::string stemOf(const std::string& name)
    {
        const std::size_t pos = name.find_last_of('.');
        return pos == std::string::npos ? name : name.substr(0, pos);
    }

    std::string extensionOf(const std::string& name)
    {
        const std::size_t pos = name.find_last_of('.');
        return pos == std::string::npos ? std::string{} : name.substr(pos);
    }

    std::vector<std::uint8_t> zlibCompress(const std::string& input)
    {
        std::vector<std::uint8_t> output(compressBound(static_cast<uLong>(input.size())));
        uLongf outSize = static_cast<uLongf>(output.size());
        if (compress2(output.data(), &outSize, reinterpret_cast<const Bytef*>(input.data()),
                static_cast<uLong>(input.size()), Z_DEFAULT_COMPRESSION)
            != Z_OK)
            throw std::runtime_error("test compression failed");
        output.resize(outSize);
        return output;
    }

    void writeArchive(const std::filesystem::path& path, const TestArchiveSpec& spec)
    {
        namespace Oblivion = Files::Oblivion;

        const bool hasFolderNames = (spec.flags & Oblivion::ArchiveFlag_FolderNames) != 0;
        const bool hasFileNames = (spec.flags & Oblivion::ArchiveFlag_FileNames) != 0;
        const bool archiveDefaultCompressed = (spec.flags & Oblivion::ArchiveFlag_CompressedFiles) != 0;

        // Group files by folder, then sort folders and files within folders by hash,
        // as required by the format when writing.
        std::map<std::uint64_t, std::vector<const TestFile*>> folders; // folder hash -> files
        std::map<std::uint64_t, std::string> folderNames; // folder hash -> stored folder name
        for (const TestFile& file : spec.files)
        {
            const std::uint64_t folderHash = testGenerateHash(file.folder, {});
            folders[folderHash].push_back(&file);
            folderNames[folderHash] = file.folder;
        }
        for (auto& [folderHash, files] : folders)
            std::sort(files.begin(), files.end(), [](const TestFile* left, const TestFile* right) {
                return testGenerateHash(stemOf(left->name), extensionOf(left->name))
                    < testGenerateHash(stemOf(right->name), extensionOf(right->name));
            });

        // On-disk size of a file's data. Does not depend on the archive flags; those
        // only decide how the compression bit in the size field is interpreted.
        const auto diskSize = [](const TestFile& file) -> std::uint32_t {
            return file.compressed ? static_cast<std::uint32_t>(4 + zlibCompress(file.data).size())
                                   : static_cast<std::uint32_t>(file.data.size());
        };

        std::uint32_t folderNamesLength = 0;
        if (hasFolderNames)
            for (const auto& [folderHash, folderName] : folderNames)
                folderNamesLength += static_cast<std::uint32_t>(folderName.size()) + 1;

        std::uint32_t fileNamesLength = 0;
        if (hasFileNames)
            for (const TestFile& file : spec.files)
                fileNamesLength += static_cast<std::uint32_t>(file.name.size()) + 1;

        std::vector<std::uint8_t> out;
        const auto u8 = [&out](std::uint8_t value) { out.push_back(value); };
        const auto u32 = [&out](std::uint32_t value) {
            for (int i = 0; i < 4; ++i)
                out.push_back(static_cast<std::uint8_t>((value >> (8 * i)) & 0xff));
        };
        const auto u64 = [&out](std::uint64_t value) {
            for (int i = 0; i < 8; ++i)
                out.push_back(static_cast<std::uint8_t>((value >> (8 * i)) & 0xff));
        };

        const std::uint32_t foldersOffset = 36;
        constexpr std::uint32_t folderRecordSize = 16;
        constexpr std::uint32_t fileRecordSize = 16;
        std::uint32_t pos = foldersOffset + folderRecordSize * static_cast<std::uint32_t>(folders.size());

        struct FolderLayout
        {
            std::uint64_t hash = 0;
            std::uint32_t fileRecordOffset = 0;
        };
        std::vector<FolderLayout> folderLayouts;
        folderLayouts.reserve(folders.size());

        // Pass 1: compute the offsets of the file record blocks.
        for (const auto& [folderHash, files] : folders)
        {
            if (hasFolderNames)
                pos += 1 + static_cast<std::uint32_t>(folderNames.at(folderHash).size()) + 1;
            folderLayouts.push_back({ folderHash, pos });
            pos += fileRecordSize * static_cast<std::uint32_t>(files.size());
        }
        // File name table follows the record blocks.
        pos += fileNamesLength;
        // File data follows the name table.
        const std::uint32_t dataOffset = pos;

        std::map<const TestFile*, std::uint32_t> dataOffsets;
        for (const auto& [folderHash, files] : folders)
            for (const TestFile* file : files)
            {
                dataOffsets[file] = pos;
                pos += diskSize(*file);
            }

        // Header.
        u32(spec.magic);
        u32(spec.version);
        u32(foldersOffset);
        u32(spec.flags);
        u32(static_cast<std::uint32_t>(folders.size()));
        u32(static_cast<std::uint32_t>(spec.files.size()));
        u32(folderNamesLength);
        u32(fileNamesLength);
        u32(spec.fileFlags);

        // Folder records.
        for (const auto& layout : folderLayouts)
        {
            u64(layout.hash);
            u32(static_cast<std::uint32_t>(folders.at(layout.hash).size()));
            u32(layout.fileRecordOffset);
        }

        // Per folder: name block (if any) and file records.
        for (const auto& layout : folderLayouts)
        {
            const auto& files = folders.at(layout.hash);
            if (hasFolderNames)
            {
                const std::string& folderName = folderNames.at(layout.hash);
                u8(static_cast<std::uint8_t>(folderName.size() + 1));
                out.insert(out.end(), folderName.begin(), folderName.end());
                u8('\0');
            }
            for (const TestFile* file : files)
            {
                u64(testGenerateHash(stemOf(file->name), extensionOf(file->name)));
                std::uint32_t sizeField = diskSize(*file);
                // Per the format, the high bit inverts the archive default compression.
                if (file->compressed != archiveDefaultCompressed)
                    sizeField |= Oblivion::FileSizeFlag_Compression;
                u32(sizeField);
                u32(dataOffsets.at(file));
            }
        }

        // File name table: bare file names, in folder record order then record order.
        if (hasFileNames)
            for (const auto& layout : folderLayouts)
                for (const TestFile* file : folders.at(layout.hash))
                {
                    out.insert(out.end(), file->name.begin(), file->name.end());
                    u8('\0');
                }

        if (out.size() != dataOffset)
            throw std::logic_error("test archive layout computation mismatch");

        // File data.
        for (const auto& [folderHash, files] : folders)
            for (const TestFile* file : files)
            {
                if (out.size() != dataOffsets.at(file))
                    throw std::logic_error("test archive data offset mismatch");
                if (file->compressed)
                {
                    const std::vector<std::uint8_t> z = zlibCompress(file->data);
                    u32(static_cast<std::uint32_t>(file->data.size()));
                    out.insert(out.end(), z.begin(), z.end());
                }
                else
                    out.insert(out.end(), file->data.begin(), file->data.end());
            }

        std::ofstream stream(path, std::ios::binary);
        stream.exceptions(std::ofstream::failbit | std::ofstream::badbit);
        stream.write(reinterpret_cast<const char*>(out.data()), static_cast<std::streamsize>(out.size()));
    }

    std::string readStreamContents(std::istream& stream)
    {
        return std::string(std::istreambuf_iterator<char>(stream), std::istreambuf_iterator<char>());
    }

    TestArchiveSpec singleFileArchive(const std::string& data, bool compressed = false)
    {
        TestArchiveSpec spec;
        spec.files.push_back({ "folder", "file.txt", data, compressed });
        return spec;
    }
} // namespace

TEST(OblivionBsa, test_header_validation_and_flags)
{
    const std::filesystem::path path = TestingOpenMW::outputFilePath("oblivion_header.bsa");

    TestArchiveSpec spec = singleFileArchive("contents", false);
    spec.fileFlags = Files::Oblivion::FileFlag_Meshes | Files::Oblivion::FileFlag_Textures;
    spec.flags |= Files::Oblivion::ArchiveFlag_CompressedFiles;
    writeArchive(path, spec);

    Files::Oblivion::BsaFile bsa;
    bsa.open(path);

    ASSERT_TRUE(bsa.isOpen());
    EXPECT_EQ(bsa.header().mFileId, Files::Oblivion::BsaFile::ArchiveMagic);
    EXPECT_EQ(bsa.header().mVersion, 0x67u);
    EXPECT_EQ(bsa.header().mFolderCount, 1u);
    EXPECT_EQ(bsa.header().mFileCount, 1u);
    EXPECT_TRUE(bsa.header().hasFolderNames());
    EXPECT_TRUE(bsa.header().hasFileNames());
    EXPECT_TRUE(bsa.header().hasDefaultCompression());
    EXPECT_TRUE(bsa.header().hasFileFlag(Files::Oblivion::FileFlag_Meshes));
    EXPECT_TRUE(bsa.header().hasFileFlag(Files::Oblivion::FileFlag_Textures));
    EXPECT_FALSE(bsa.header().hasFileFlag(Files::Oblivion::FileFlag_Sounds));
    ASSERT_EQ(bsa.getList().size(), 1u);
    EXPECT_EQ(std::string(bsa.getList().front().name()), "folder\\file.txt");
}

TEST(OblivionBsa, test_extraction_uncompressed)
{
    const std::filesystem::path path = TestingOpenMW::outputFilePath("oblivion_extract.bsa");

    std::string payload;
    for (int i = 0; i < 512; ++i)
        payload += static_cast<char>(i & 0xff); // includes embedded NULs

    writeArchive(path, singleFileArchive(payload));

    Files::Oblivion::BsaFile bsa;
    bsa.open(path);

    const Files::Oblivion::BsaFile::FileRecord* record = bsa.findFile("folder/file.txt");
    ASSERT_NE(record, nullptr);
    EXPECT_EQ(readStreamContents(*bsa.getFile(*record)), payload);

    // Lookup is case insensitive and separator agnostic.
    EXPECT_NE(bsa.findFile("FOLDER\\FILE.TXT"), nullptr);
    EXPECT_NE(bsa.findFile("folder\\file.txt"), nullptr);
    EXPECT_EQ(bsa.findFile("folder/missing.txt"), nullptr);
    EXPECT_TRUE(bsa.contains("folder\\file.txt"));
    EXPECT_FALSE(bsa.contains("folder\\missing.txt"));

    // VFS compatible listing and extraction path.
    ASSERT_EQ(bsa.getList().size(), 1u);
    Files::IStreamPtr stream = bsa.getFile(&bsa.getList().front());
    EXPECT_EQ(readStreamContents(*stream), payload);
}

TEST(OblivionBsa, test_extraction_compressed_by_archive_default)
{
    const std::filesystem::path path = TestingOpenMW::outputFilePath("oblivion_compressed_default.bsa");

    std::string payload;
    for (int i = 0; i < 2048; ++i)
        payload += "Oblivion!";

    TestArchiveSpec spec;
    spec.files.push_back({ "folder", "file.txt", payload, true }); // stored compressed
    spec.flags |= Files::Oblivion::ArchiveFlag_CompressedFiles;
    writeArchive(path, spec);

    Files::Oblivion::BsaFile bsa;
    bsa.open(path);

    const Files::Oblivion::BsaFile::FileRecord* record = bsa.findFile("folder/file.txt");
    ASSERT_NE(record, nullptr);
    EXPECT_TRUE(record->isCompressed(true));
    EXPECT_EQ(readStreamContents(*bsa.getFile(*record)), payload);
}

TEST(OblivionBsa, test_extraction_compressed_per_file_flag)
{
    const std::filesystem::path path = TestingOpenMW::outputFilePath("oblivion_compressed_per_file.bsa");

    TestArchiveSpec spec; // archive default: not compressed
    spec.files.push_back({ "folder", "compressed.txt", std::string(1024, 'x'), true });
    spec.files.push_back({ "folder", "plain.txt", "plain contents", false });
    writeArchive(path, spec);

    Files::Oblivion::BsaFile bsa;
    bsa.open(path);

    const Files::Oblivion::BsaFile::FileRecord* compressed = bsa.findFile("folder/compressed.txt");
    const Files::Oblivion::BsaFile::FileRecord* plain = bsa.findFile("folder/plain.txt");
    ASSERT_NE(compressed, nullptr);
    ASSERT_NE(plain, nullptr);
    EXPECT_TRUE(compressed->isCompressed(false));
    EXPECT_FALSE(plain->isCompressed(false));
    EXPECT_EQ(readStreamContents(*bsa.getFile(*compressed)), std::string(1024, 'x'));
    EXPECT_EQ(readStreamContents(*bsa.getFile(*plain)), "plain contents");
}

TEST(OblivionBsa, test_extraction_uncompressed_file_in_compressed_archive)
{
    const std::filesystem::path path = TestingOpenMW::outputFilePath("oblivion_compressed_archive_plain_file.bsa");

    TestArchiveSpec spec;
    spec.files.push_back({ "folder", "file.txt", "stored without compression", false }); // stored raw
    spec.flags |= Files::Oblivion::ArchiveFlag_CompressedFiles;
    writeArchive(path, spec);

    Files::Oblivion::BsaFile bsa;
    bsa.open(path);

    const Files::Oblivion::BsaFile::FileRecord* record = bsa.findFile("folder/file.txt");
    ASSERT_NE(record, nullptr);
    EXPECT_FALSE(record->isCompressed(true));
    EXPECT_EQ(readStreamContents(*bsa.getFile(*record)), "stored without compression");
}

TEST(OblivionBsa, test_rejects_other_versions)
{
    for (const std::uint32_t version : { 0x68u, 0x69u })
    {
        const std::filesystem::path path = TestingOpenMW::outputFilePath(
            "oblivion_wrong_version_" + std::to_string(version) + ".bsa");

        TestArchiveSpec spec = singleFileArchive("data");
        spec.version = version; // Fallout 3 / Skyrim SE
        writeArchive(path, spec);

        Files::Oblivion::BsaFile bsa;
        EXPECT_THROW(bsa.open(path), std::runtime_error);
    }
}

TEST(OblivionBsa, test_rejects_other_magic)
{
    const std::filesystem::path path = TestingOpenMW::outputFilePath("oblivion_wrong_magic.bsa");

    TestArchiveSpec spec = singleFileArchive("data");
    spec.magic = 0x100; // Morrowind uncompressed BSA magic
    writeArchive(path, spec);

    Files::Oblivion::BsaFile bsa;
    EXPECT_THROW(bsa.open(path), std::runtime_error);
}

TEST(OblivionBsa, test_rejects_xbox360_archive)
{
    const std::filesystem::path path = TestingOpenMW::outputFilePath("oblivion_xbox360.bsa");

    TestArchiveSpec spec = singleFileArchive("data");
    spec.flags |= Files::Oblivion::ArchiveFlag_Xbox360;
    writeArchive(path, spec);

    Files::Oblivion::BsaFile bsa;
    EXPECT_THROW(bsa.open(path), std::runtime_error);
}

TEST(OblivionBsa, test_rejects_truncated_archive)
{
    const std::filesystem::path path = TestingOpenMW::outputFilePath("oblivion_truncated.bsa");

    writeArchive(path, singleFileArchive(std::string(4096, 'y')));

    // Truncate the archive partway through the file data.
    std::filesystem::resize_file(path, std::filesystem::file_size(path) - 100);

    Files::Oblivion::BsaFile bsa;
    EXPECT_THROW(bsa.open(path), std::runtime_error);
}

TEST(OblivionBsa, test_rejects_file_pointing_outside_archive)
{
    const std::filesystem::path path = TestingOpenMW::outputFilePath("oblivion_bad_offset.bsa");

    writeArchive(path, singleFileArchive("data"));

    // The single file record comes right after the header (36) and the folder record (16)
    // and the folder name block (1 + 6 + 1). Its offset field sits at record + 8 + 4.
    const std::uint64_t fileRecordOffset = 36 + 16 + 1 + std::string("folder").size() + 1;
    const std::uint64_t offsetFieldOffset = fileRecordOffset + 8 + 4;

    std::fstream stream(path, std::ios::binary | std::ios::in | std::ios::out);
    ASSERT_TRUE(stream.is_open());
    stream.seekp(static_cast<std::streamoff>(offsetFieldOffset));
    const std::uint8_t bogus[4] = { 0xff, 0xff, 0xff, 0x7f }; // 0x7fffffff, little endian
    stream.write(reinterpret_cast<const char*>(bogus), sizeof(bogus));
    stream.close();

    Files::Oblivion::BsaFile bsa;
    EXPECT_THROW(bsa.open(path), std::runtime_error);
}

TEST(OblivionBsa, test_extraction_cache)
{
    const std::filesystem::path path = TestingOpenMW::outputFilePath("oblivion_cache.bsa");

    const std::string payload(100, 'z');
    writeArchive(path, singleFileArchive(payload));

    Files::Oblivion::BsaFile bsa;
    bsa.open(path);

    EXPECT_EQ(bsa.cacheLimit(), Files::Oblivion::BsaFile::DefaultCacheLimit);
    EXPECT_EQ(bsa.cacheSize(), 0u);
    EXPECT_EQ(bsa.cacheEntryCount(), 0u);

    const Files::Oblivion::BsaFile::FileRecord* record = bsa.findFile("folder/file.txt");
    ASSERT_NE(record, nullptr);

    EXPECT_EQ(readStreamContents(*bsa.getFile(*record)), payload);
    EXPECT_EQ(bsa.cacheEntryCount(), 1u);
    EXPECT_EQ(bsa.cacheSize(), payload.size());

    // The second read is served from the cache.
    EXPECT_EQ(readStreamContents(*bsa.getFile(*record)), payload);
    EXPECT_EQ(bsa.cacheEntryCount(), 1u);

    bsa.clearCache();
    EXPECT_EQ(bsa.cacheEntryCount(), 0u);
    EXPECT_EQ(bsa.cacheSize(), 0u);

    // A cache limit of zero disables caching.
    bsa.setCacheLimit(0);
    EXPECT_EQ(readStreamContents(*bsa.getFile(*record)), payload);
    EXPECT_EQ(bsa.cacheEntryCount(), 0u);

    // A budget smaller than the file means the file cannot be cached.
    bsa.setCacheLimit(8);
    EXPECT_EQ(readStreamContents(*bsa.getFile(*record)), payload);
    EXPECT_EQ(bsa.cacheEntryCount(), 0u);

    bsa.setCacheLimit(Files::Oblivion::BsaFile::DefaultCacheLimit);
    EXPECT_EQ(readStreamContents(*bsa.getFile(*record)), payload);
    EXPECT_EQ(bsa.cacheEntryCount(), 1u);
}

TEST(OblivionBsa, test_archive_without_file_names)
{
    const std::filesystem::path path = TestingOpenMW::outputFilePath("oblivion_no_file_names.bsa");

    TestArchiveSpec spec = singleFileArchive("contents");
    spec.flags = Files::Oblivion::ArchiveFlag_FolderNames; // no file name table
    writeArchive(path, spec);

    Files::Oblivion::BsaFile bsa;
    bsa.open(path);

    // Without a file name table the flat listing is empty...
    EXPECT_TRUE(bsa.getList().empty());

    // ...but point lookups by name still work, because the file and folder hashes
    // are computed from the requested path.
    const Files::Oblivion::BsaFile::FileRecord* record = bsa.findFile("folder/file.txt");
    ASSERT_NE(record, nullptr);
    EXPECT_EQ(readStreamContents(*bsa.getFile(*record)), "contents");
}

TEST(OblivionBsa, test_archive_without_folder_names)
{
    const std::filesystem::path path = TestingOpenMW::outputFilePath("oblivion_no_folder_names.bsa");

    TestArchiveSpec spec;
    spec.files.push_back({ "", "file.txt", "contents", false }); // root level file
    spec.flags = Files::Oblivion::ArchiveFlag_FileNames; // no folder name table
    writeArchive(path, spec);

    Files::Oblivion::BsaFile bsa;
    bsa.open(path);

    ASSERT_EQ(bsa.getList().size(), 1u);
    EXPECT_EQ(std::string(bsa.getList().front().name()), "file.txt"); // no folder prefix available
    const Files::Oblivion::BsaFile::FileRecord* record = bsa.findFile("file.txt");
    ASSERT_NE(record, nullptr);
    EXPECT_EQ(readStreamContents(*bsa.getFile(*record)), "contents");
}

TEST(OblivionBsa, test_without_folder_names_full_path_lookup_still_works)
{
    const std::filesystem::path path = TestingOpenMW::outputFilePath("oblivion_folderless_names.bsa");

    TestArchiveSpec spec; // file stored under a folder, but no folder name table
    spec.files.push_back({ "folder", "file.txt", "contents", false });
    spec.flags = Files::Oblivion::ArchiveFlag_FileNames;
    writeArchive(path, spec);

    Files::Oblivion::BsaFile bsa;
    bsa.open(path);

    // The root folder does not exist in the archive.
    EXPECT_EQ(bsa.findFile("file.txt"), nullptr);

    // Lookup by full path still works: the folder hash is computed from the requested
    // path and does not depend on the stored folder name. The stored record name is
    // bare, because the folder prefix cannot be recovered.
    const Files::Oblivion::BsaFile::FileRecord* record = bsa.findFile("folder/file.txt");
    ASSERT_NE(record, nullptr);
    EXPECT_EQ(record->name(), "file.txt");
    EXPECT_EQ(readStreamContents(*bsa.getFile(*record)), "contents");
}

TEST(OblivionBsa, test_empty_archive)
{
    const std::filesystem::path path = TestingOpenMW::outputFilePath("oblivion_empty.bsa");

    writeArchive(path, TestArchiveSpec{});

    Files::Oblivion::BsaFile bsa;
    bsa.open(path);

    ASSERT_TRUE(bsa.isOpen());
    EXPECT_EQ(bsa.header().mFolderCount, 0u);
    EXPECT_EQ(bsa.header().mFileCount, 0u);
    EXPECT_TRUE(bsa.getList().empty());
}

TEST(OblivionBsa, test_normalize_name)
{
    using Files::Oblivion::BsaFile;
    EXPECT_EQ(BsaFile::normalizeName("folder/file.txt"), "folder\\file.txt");
    EXPECT_EQ(BsaFile::normalizeName("FOLDER\\SUB/FILE.TXT"), "folder\\sub\\file.txt");
    EXPECT_EQ(BsaFile::normalizeName("\\\\root\\\\dir\\\\file.txt"), "root\\dir\\file.txt");
    EXPECT_EQ(BsaFile::normalizeName("a\\\\b//c\\\\\\d.nif"), "a\\b\\c\\d.nif");
    EXPECT_EQ(BsaFile::normalizeName("trailing\\\\"), "trailing");
    EXPECT_EQ(BsaFile::normalizeName(""), "");
    EXPECT_EQ(BsaFile::normalizeName("/"), "");
}

TEST(OblivionBsa, test_generate_hash_extension_flags)
{
    using Files::Oblivion::BsaFile;

    const std::uint64_t base = BsaFile::generateHash("cuirass", {});
    EXPECT_EQ(BsaFile::generateHash("", {}), 0u);
    EXPECT_EQ(BsaFile::generateHash("cuirass", ".nif") & 0x8000, 0x8000u);
    EXPECT_EQ(BsaFile::generateHash("cuirass", ".kf") & 0x80, 0x80u);
    EXPECT_EQ(BsaFile::generateHash("cuirass", ".dds") & 0x8080, 0x8080u);
    EXPECT_EQ(BsaFile::generateHash("cuirass", ".wav") & 0x80000000, 0x80000000u);
    EXPECT_NE(BsaFile::generateHash("cuirass", ".nif"), base);
}

TEST(OblivionBsa, test_multiple_files_and_root_level_files)
{
    const std::filesystem::path path = TestingOpenMW::outputFilePath("oblivion_multifile.bsa");

    TestArchiveSpec spec;
    spec.files.push_back({ "textures", "stone.dds", std::string(2048, 's'), false });
    spec.files.push_back({ "meshes", "wall.nif", "nif payload", true });
    spec.files.push_back({ "", "readme.txt", "root level file", false });
    spec.files.push_back({ "meshes", "armor_cuirass.nif", "armor payload", false });
    writeArchive(path, spec);

    Files::Oblivion::BsaFile bsa;
    bsa.open(path);

    EXPECT_EQ(bsa.header().mFolderCount, 3u);
    EXPECT_EQ(bsa.header().mFileCount, 4u);
    EXPECT_EQ(bsa.getList().size(), 4u);

    // Root level file has no folder prefix.
    const Files::Oblivion::BsaFile::FileRecord* readme = bsa.findFile("readme.txt");
    ASSERT_NE(readme, nullptr);
    EXPECT_EQ(readme->name(), "readme.txt");

    EXPECT_EQ(readStreamContents(*bsa.getFile(*bsa.findFile("meshes/wall.nif"))), "nif payload");
    EXPECT_EQ(readStreamContents(*bsa.getFile(*bsa.findFile("meshes/armor_cuirass.nif"))), "armor payload");
    EXPECT_EQ(readStreamContents(*bsa.getFile(*bsa.findFile("textures/stone.dds"))), std::string(2048, 's'));
}
