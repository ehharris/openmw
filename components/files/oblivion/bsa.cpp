/*
  Copyright (C) 2026 The OpenMW Developers

  This software is provided 'as-is', without any express or implied
  warranty.  In no event will the authors be held liable for any damages
  arising from the use of this software.

  Permission is granted to anyone to use this software for any purpose,
  including commercial applications, and to alter it and redistribute it
  freely, subject to the following restrictions:

  1. The origin of this software must not be misrepresented; you must not
     claim that you wrote the original software. If you use this software
     in a product, an acknowledgment in the product documentation would be
     appreciated but is not required.
  2. Altered source versions must be plainly marked as such, and must not be
     misrepresented as being the original software.
  3. This notice may not be removed or altered from any source distribution.

*/
#include "bsa.hpp"

#include <algorithm>
#include <cstring>
#include <format>
#include <fstream>
#include <istream>
#include <memory>
#include <stdexcept>
#include <utility>
#include <vector>

#include <zlib.h>

#include <components/files/conversion.hpp>
#include <components/files/memorystream.hpp>
#include <components/files/utils.hpp>
#include <components/misc/pathhelpers.hpp>

namespace Files
{
    namespace Oblivion
    {
        namespace
        {
            // Shared, zero copy istream over an extracted file. MemBuf is a
            // non owning streambuf; the shared_ptr member keeps the data alive
            // for as long as the stream exists.
            class SharedMemoryInputStream : private virtual Files::MemBuf, public std::istream
            {
            public:
                explicit SharedMemoryInputStream(std::shared_ptr<const std::vector<char>> data)
                    : Files::MemBuf(data ? data->data() : nullptr, data ? data->size() : 0)
                    , std::istream(static_cast<std::streambuf*>(this))
                    , mData(std::move(data))
                {
                }

            private:
                std::shared_ptr<const std::vector<char>> mData;
            };

        } // namespace

        BsaFile::~BsaFile()
        {
            close();
        }

        [[noreturn]] void BsaFile::fail(const std::string& msg) const
        {
            throw std::runtime_error(
                "BSA Error: " + msg + "\nArchive: " + Files::pathToUnicodeString(mFilepath));
        }

        std::string BsaFile::normalizeName(std::string_view name)
        {
            std::string out;
            out.reserve(name.size());
            for (const char c : name)
            {
                if (c == '/' || c == '\\')
                {
                    // Keep exactly one separator, never at the front.
                    if (!out.empty() && out.back() != '\\')
                        out.push_back('\\');
                    continue;
                }
                if (c >= 'A' && c <= 'Z')
                    out.push_back(static_cast<char>(c - 'A' + 'a'));
                else
                    out.push_back(c);
            }
            while (!out.empty() && out.back() == '\\')
                out.pop_back();
            return out;
        }

        std::uint64_t BsaFile::generateHash(std::string_view str, std::string_view extension)
        {
            if (str.empty())
                return 0;
            const auto at = [&](std::size_t i) -> unsigned char {
                const char c = str[i];
                if (c == '/')
                    return '\\';
                return static_cast<unsigned char>(c);
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

        const BsaFile::Header& BsaFile::header() const
        {
            if (!mOpen)
                fail("Archive is not open");
            return mHeader;
        }

        std::string BsaFile::getFilename() const
        {
            return Files::pathToUnicodeString(mFilepath);
        }

        void BsaFile::readHeader(std::istream& input)
        {
            const auto readExact = [this, &input](void* data, std::size_t size, const char* what) {
                input.read(reinterpret_cast<char*>(data), static_cast<std::streamsize>(size));
                if (!input || input.gcount() != static_cast<std::streamsize>(size))
                    fail(std::format("Failed to read {}", what));
            };
            const auto readU8 = [&readExact]() {
                std::uint8_t value = 0;
                readExact(&value, sizeof(value), "a byte");
                return value;
            };
            const auto readU32LE = [&readExact]() {
                std::uint8_t bytes[4];
                readExact(bytes, sizeof(bytes), "a 32 bit value");
                return static_cast<std::uint32_t>(bytes[0]) | (static_cast<std::uint32_t>(bytes[1]) << 8)
                    | (static_cast<std::uint32_t>(bytes[2]) << 16) | (static_cast<std::uint32_t>(bytes[3]) << 24);
            };
            const auto readU64LE = [&readExact]() {
                std::uint8_t bytes[8];
                readExact(bytes, sizeof(bytes), "a 64 bit value");
                std::uint64_t value = 0;
                for (int i = 7; i >= 0; --i)
                    value = (value << 8) | bytes[i];
                return value;
            };
            const auto readBytes = [&readExact](std::size_t size) {
                std::vector<char> out(size);
                if (size > 0)
                    readExact(out.data(), size, "a byte string");
                return out;
            };

            input.seekg(0);
            if (!input)
                fail("Failed to seek to the start of the archive");

            // --- 36 byte header
            mHeader = Header{};
            mHeader.mFileId = readU32LE();
            if (mHeader.mFileId != ArchiveMagic)
                fail("Unrecognized archive magic (expected \"BSA\\0\")");
            mHeader.mVersion = readU32LE();
            if (mHeader.mVersion != ArchiveVersion)
                fail(std::format(
                    "Unsupported archive version {} (expected {} - Oblivion)", mHeader.mVersion, ArchiveVersion));
            mHeader.mFoldersOffset = readU32LE();
            mHeader.mFlags = readU32LE();
            mHeader.mFolderCount = readU32LE();
            mHeader.mFileCount = readU32LE();
            mHeader.mFolderNamesLength = readU32LE();
            mHeader.mFileNamesLength = readU32LE();
            mHeader.mFileFlags = readU32LE();

            if (mHeader.isXbox360())
                fail("Xbox 360 Oblivion archives (big endian layout) are not supported");
            const std::uint64_t fsize = mArchiveSize;
            if (mHeader.mFoldersOffset < sizeof(Header) || mHeader.mFoldersOffset > fsize)
                fail(std::format("Folder record offset {} is out of bounds", mHeader.mFoldersOffset));
            const std::uint64_t foldersEnd = static_cast<std::uint64_t>(mHeader.mFoldersOffset)
                + 16ull * mHeader.mFolderCount;
            if (foldersEnd > fsize)
                fail("Folder record table is larger than the archive");
            // Every folder record and file record takes 16 bytes; reject absurd counts up front.
            if (mHeader.mFolderCount > fsize / 16 || mHeader.mFileCount > fsize / 16)
                fail("Archive header reports more folders/files than fit in the archive");
            if (mHeader.mFolderNamesLength > fsize || mHeader.mFileNamesLength > fsize)
                fail("Name tables are larger than the archive");

            // --- folder records (16 bytes each)
            input.seekg(mHeader.mFoldersOffset);
            if (!input)
                fail("Failed to seek to the folder records");

            struct ParsedFolder
            {
                std::uint64_t mHash = 0;
                std::uint32_t mCount = 0;
                std::uint32_t mOffset = 0;
                std::string mName;
                std::vector<FileRecord> mFileRecords;
            };

            std::vector<ParsedFolder> parsed;
            parsed.reserve(mHeader.mFolderCount);
            std::uint64_t totalFileRecords = 0;
            for (std::uint32_t i = 0; i < mHeader.mFolderCount; ++i)
            {
                ParsedFolder folder;
                folder.mHash = readU64LE();
                folder.mCount = readU32LE();
                folder.mOffset = readU32LE(); // informational, the file data offsets are absolute
                totalFileRecords += folder.mCount;
                if (totalFileRecords > fsize / 16)
                    fail("Folder records reference more files than fit in the archive");
                parsed.push_back(std::move(folder));
            }

            // --- per folder: folder name block (optional) and file records
            std::uint32_t folderNamesRemaining = mHeader.mFolderNamesLength;
            const bool hasFolderNames = mHeader.hasFolderNames();
            const bool hasFileNames = mHeader.hasFileNames();
            const bool archiveDefaultCompressed = mHeader.hasDefaultCompression();
            std::uint64_t recordsRead = 0;
            for (auto& folder : parsed)
            {
                if (hasFolderNames)
                {
                    const std::uint8_t nameLength = readU8();
                    if (nameLength > folderNamesRemaining)
                        fail("Folder name table is smaller than the declared folder name");
                    folderNamesRemaining -= nameLength;
                    if (nameLength > 0)
                    {
                        std::vector<char> nameBytes = readBytes(nameLength);
                        if (nameBytes.back() != '\0')
                            fail("Folder name is not NUL terminated");
                        folder.mName.assign(nameBytes.data(), nameBytes.size() - 1);
                    }
                }

                folder.mFileRecords.reserve(folder.mCount);
                for (std::uint32_t i = 0; i < folder.mCount; ++i)
                {
                    FileRecord fileRecord;
                    fileRecord.mHash = readU64LE();
                    fileRecord.mSize = readU32LE();
                    fileRecord.mOffset = readU32LE();

                    const std::uint64_t dataEnd
                        = static_cast<std::uint64_t>(fileRecord.mOffset) + fileRecord.sizeOnDisk();
                    if (fileRecord.mOffset > fsize || dataEnd > fsize)
                        fail(std::format("File record {} points outside the archive", recordsRead + i));
                    if (fileRecord.isCompressed(archiveDefaultCompressed) && fileRecord.sizeOnDisk() < 4)
                        fail(std::format("File record {} is compressed but smaller than the size prefix",
                            recordsRead + i));

                    folder.mFileRecords.push_back(std::move(fileRecord));
                }
                recordsRead += folder.mCount;
            }

            // --- file name table (optional)
            if (hasFileNames)
            {
                std::uint32_t fileNamesRemaining = mHeader.mFileNamesLength;
                for (auto& folder : parsed)
                    for (auto& fileRecord : folder.mFileRecords)
                    {
                        std::vector<char> nameBytes;
                        for (;;)
                        {
                            if (fileNamesRemaining == 0)
                                fail("File name table is smaller than the declared file names");
                            const char c = static_cast<char>(readU8());
                            --fileNamesRemaining;
                            if (c == '\0')
                                break;
                            nameBytes.push_back(c);
                            if (nameBytes.size() > 255)
                                fail("File name is too long (missing NUL terminator within 255 bytes)");
                        }
                        // Full stored path: folder\file, or just file for root level entries.
                        if (!folder.mName.empty())
                        {
                            fileRecord.mName.assign(folder.mName.begin(), folder.mName.end());
                            fileRecord.mName.push_back('\\');
                        }
                        fileRecord.mName.insert(fileRecord.mName.end(), nameBytes.begin(), nameBytes.end());
                    }
            }
            // Any unread remainder of the name tables is ignored on purpose;
            // well formed archives do not leave any bytes behind.

            // --- build the lookup maps
            mFolders.clear();
            mFiles.clear();
            mFileStructMap.clear();

            for (auto& folder : parsed)
            {
                FolderRecord record;
                record.mHash = folder.mHash;
                record.mCount = folder.mCount;
                record.mOffset = folder.mOffset;
                record.mName = std::move(folder.mName);
                for (auto& fileRecord : folder.mFileRecords)
                {
                    fileRecord.mFolderHash = folder.mHash;
                    record.mFiles[fileRecord.mHash] = std::move(fileRecord);
                }
                mFolders[record.mHash] = std::move(record);
            }

            // Build the flat, VFS compatible file list. Records from archives
            // without a file name table carry no usable name and are omitted.
            std::vector<std::pair<Bsa::BSAFile::FileStruct, const FileRecord*>> pending;
            pending.reserve(static_cast<std::size_t>(recordsRead));
            for (auto& [folderHash, folder] : mFolders)
                for (auto& [fileHash, fileRecord] : folder.mFiles)
                {
                    if (fileRecord.mName.empty())
                        continue;
                    Bsa::BSAFile::FileStruct fs;
                    fs.mFileSize = fileRecord.sizeOnDisk();
                    fs.mOffset = fileRecord.mOffset;
                    fs.mHash = Bsa::BSAFile::Hash{
                        static_cast<std::uint32_t>(fileRecord.mHash & 0xffffffffu),
                        static_cast<std::uint32_t>(fileRecord.mHash >> 32)
                    };
                    fs.mNameOffset = 0;
                    fs.mNameSize = static_cast<std::uint32_t>(fileRecord.mName.size());
                    fs.mNamesBuffer = &fileRecord.mName;
                    pending.emplace_back(std::move(fs), &fileRecord);
                }

            std::sort(pending.begin(), pending.end(), [](const auto& left, const auto& right) {
                return left.first.name() < right.first.name();
            });

            // mFiles never reallocates after this point, so the pointers stored
            // in mFileStructMap stay valid.
            mFiles.reserve(pending.size());
            for (auto& [fs, fileRecord] : pending)
            {
                mFiles.push_back(std::move(fs));
                mFileStructMap.emplace(&mFiles.back(), fileRecord);
            }
        }

        void BsaFile::open(const std::filesystem::path& path)
        {
            if (mOpen)
                close();

            mFilepath = path;
            mOpen = false;

            std::ifstream input(mFilepath, std::ios::binary);
            if (!input.is_open())
                fail("Unable to open archive file");

            const std::streamsize size = Files::getStreamSizeLeft(input);
            if (size < 0)
                fail("Unable to determine the archive size");
            mArchiveSize = static_cast<std::uint64_t>(size);
            if (mArchiveSize < sizeof(Header))
                fail(std::format("File too small to be an Oblivion BSA archive ({} bytes)", mArchiveSize));

            try
            {
                readHeader(input);
            }
            catch (...)
            {
                close();
                mFilepath = path; // keep the path around for error reporting after a failed open
                throw;
            }

            mOpen = true;
        }

        void BsaFile::close()
        {
            clearCache();
            mFileStructMap.clear();
            mFiles.clear();
            mFolders.clear();
            mHeader = Header{};
            mArchiveSize = 0;
            mOpen = false;
        }

        void BsaFile::setCacheLimit(std::size_t bytes)
        {
            std::lock_guard lock(mCacheMutex);
            mCacheLimit = bytes;
            if (mCacheLimit == 0)
            {
                mCache.clear();
                mCacheOrder.clear();
                mCacheBytes = 0;
                return;
            }
            while (mCacheBytes > mCacheLimit && !mCacheOrder.empty())
            {
                const std::uint64_t oldest = mCacheOrder.front();
                mCacheOrder.pop_front();
                const auto it = mCache.find(oldest);
                if (it == mCache.end())
                    continue;
                mCacheBytes -= it->second->size();
                mCache.erase(it);
            }
        }

        void BsaFile::clearCache()
        {
            std::lock_guard lock(mCacheMutex);
            mCache.clear();
            mCacheOrder.clear();
            mCacheBytes = 0;
        }

        void BsaFile::insertIntoCache(const std::uint64_t key, std::shared_ptr<const std::vector<char>> data) const
        {
            if (mCacheLimit == 0)
                return;
            const std::size_t bytes = data->size();
            if (bytes > mCacheLimit)
                return; // too large to ever fit, do not poison the cache with it

            std::lock_guard lock(mCacheMutex);
            if (mCache.contains(key))
                return; // someone beat us to it, keep the existing entry for stability
            mCache.emplace(key, std::move(data));
            mCacheOrder.push_back(key);
            mCacheBytes += bytes;
            while (mCacheBytes > mCacheLimit && !mCacheOrder.empty())
            {
                const std::uint64_t oldest = mCacheOrder.front();
                mCacheOrder.pop_front();
                const auto it = mCache.find(oldest);
                if (it == mCache.end())
                    continue;
                mCacheBytes -= it->second->size();
                mCache.erase(it);
            }
        }

        const BsaFile::FileRecord* BsaFile::findFile(std::string_view name) const
        {
            for (const unsigned char c : name)
            {
                if ((c & 0x80) != 0)
                {
                    fail(std::format("File name {} contains unicode characters, refusing to look it up", name));
                }
            }

            const std::string canonical = normalizeName(name);
            if (canonical.empty())
                return nullptr;

            std::string_view rest = canonical;
            const auto separator = rest.find_last_of('\\');
            std::string_view folder;
            if (separator != std::string_view::npos)
                folder = rest.substr(0, separator);
            if (separator != std::string_view::npos)
                rest.remove_prefix(separator + 1);
            const std::string_view file = rest;
            if (file.empty())
                return nullptr;

            const std::uint64_t folderHash = generateHash(folder, {});
            const std::string_view stem = Misc::stemFile(file);
            std::string_view extension;
            if (const std::size_t pos = Misc::findExtension(file); pos != std::string_view::npos)
            {
                extension = file;
                extension.remove_prefix(pos); // includes the leading dot
            }

            const std::uint64_t fileHash = generateHash(stem, extension);
            return findFileByHash(folderHash, fileHash);
        }

        const BsaFile::FileRecord* BsaFile::findFileByHash(std::uint64_t folderHash, std::uint64_t fileHash) const
        {
            const auto folderIt = mFolders.find(folderHash);
            if (folderIt == mFolders.end())
                return nullptr;
            const auto fileIt = folderIt->second.mFiles.find(fileHash);
            if (fileIt == folderIt->second.mFiles.end())
                return nullptr;
            return &fileIt->second;
        }

        Files::IStreamPtr BsaFile::getFile(const FileRecord& record) const
        {
            if (findFileByHash(record.mFolderHash, record.mHash) != &record)
                fail("File record does not belong to this archive");

            const std::uint64_t key = (static_cast<std::uint64_t>(record.mSize) << 32) | record.mOffset;
            {
                std::lock_guard lock(mCacheMutex);
                const auto it = mCache.find(key);
                if (it != mCache.end())
                    return std::make_unique<SharedMemoryInputStream>(it->second);
            }

            std::shared_ptr<const std::vector<char>> data = extract(record);
            insertIntoCache(key, data);
            return std::make_unique<SharedMemoryInputStream>(std::move(data));
        }

        Files::IStreamPtr BsaFile::getFile(const Bsa::BSAFile::FileStruct* file) const
        {
            const auto it = mFileStructMap.find(file);
            if (it == mFileStructMap.end())
                fail("File struct does not belong to this archive");
            return getFile(*it->second);
        }

        std::shared_ptr<const std::vector<char>> BsaFile::extract(const FileRecord& record) const
        {
            if (!mOpen)
                fail("Archive is not open");

            const std::uint32_t sizeOnDisk = record.sizeOnDisk();
            const std::uint64_t offset = record.mOffset;
            if (offset > mArchiveSize || static_cast<std::uint64_t>(sizeOnDisk) > mArchiveSize - offset)
                fail(std::format("File {} points outside the archive", record.name()));

            std::ifstream input(mFilepath, std::ios::binary);
            if (!input.is_open())
                fail("Unable to open archive file for reading");
            input.seekg(static_cast<std::streamoff>(offset));
            if (!input)
                fail(std::format("Unable to seek to file {}", record.name()));

            auto diskBytes = std::make_shared<std::vector<char>>(sizeOnDisk);
            if (sizeOnDisk > 0)
            {
                input.read(diskBytes->data(), static_cast<std::streamsize>(sizeOnDisk));
                if (!input || input.gcount() != static_cast<std::streamsize>(sizeOnDisk))
                    fail(std::format("Failed to read file {}", record.name()));
            }

            const bool compressed = record.isCompressed(mHeader.hasDefaultCompression());
            if (!compressed)
                return diskBytes;

            if (sizeOnDisk < sizeof(std::uint32_t))
                fail(std::format("Compressed file {} is too small", record.name()));

            std::uint32_t uncompressedSize = 0;
            {
                std::uint8_t bytes[4];
                std::memcpy(bytes, diskBytes->data(), sizeof(bytes));
                for (int i = 3; i >= 0; --i)
                    uncompressedSize = (uncompressedSize << 8) | bytes[i];
            }
            const std::uint32_t compressedSize = sizeOnDisk - sizeof(std::uint32_t);
            // zlib's worst case expansion ratio is ~1032, so reject absurd sizes up front.
            if (static_cast<std::uint64_t>(uncompressedSize) > static_cast<std::uint64_t>(compressedSize) * 1032)
                fail(std::format("Compressed file {} expands to an invalid size", record.name()));
            if (uncompressedSize == 0)
                return std::make_shared<std::vector<char>>();

            auto outBytes = std::make_shared<std::vector<char>>(uncompressedSize);
            uLongf destSize = static_cast<uLongf>(uncompressedSize);
            const int ec = ::uncompress(reinterpret_cast<Bytef*>(outBytes->data()), &destSize,
                reinterpret_cast<const Bytef*>(diskBytes->data() + sizeof(std::uint32_t)),
                static_cast<uLong>(compressedSize));
            if (ec != Z_OK)
                fail(std::format("zlib uncompress failed for file {}: {}", record.name(), ::zError(ec)));
            if (destSize != uncompressedSize)
                fail(std::format(
                    "Decompressed file {} has size {}, expected {}", record.name(), destSize, uncompressedSize));
            return outBytes;
        }
    } // namespace Oblivion
} // namespace Files
