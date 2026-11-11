#ifndef OPENMW_COMPONENTS_FILES_OBLIVION_BSA_H
#define OPENMW_COMPONENTS_FILES_OBLIVION_BSA_H

/*
  Dedicated reader for Oblivion (TES4, version 103) BSA archives.

  Oblivion BSAs share the "BSA\0" magic and the overall TES4-era layout with the
  Fallout 3 / Skyrim families, but the version number in the second header word
  distinguishes them (0x67, 103 - see https://en.uesp.net/wiki/Oblivion_Mod:BSA_File_Format).
  This class implements exactly the Oblivion flavour of the format:

    - 36 byte header (see Header),
    - 16 byte folder records (name hash, file count, offset, little endian),
    - 16 byte file records (name hash, size, absolute offset, little endian),
    - optional folder name and file name tables (ArchiveFlag_FolderNames /
      ArchiveFlag_FileNames; both are set in every archive shipped by Oblivion),
    - zlib-compressed files when ArchiveFlag_CompressedFiles is set or the high
      bit (FileSizeFlag_Compression) of a file's size is set,
    - Oblivion-specific archive and file flags (see ArchiveFlag / FileFlag).

  Not supported, on purpose:

    - Xbox 360 (big endian) archives, ArchiveFlag_Xbox360 - opening one fails
      with a descriptive error instead of mis-parsing the contents,
    - versions other than 103 (Fallout 3 = 104, Skyrim SE = 105, Morrowind = 0x100).
      Use Bsa::BSAFile / Bsa::CompressedBSAFile for those,
    - BSA v104+ semantics of ArchiveFlag_Unknown100 / ArchiveFlag_Unknown200
      ("embedded names" / XMEM codec). In Oblivion these bits are set in all
      official archives and carry no structural meaning, so they are ignored.
    - writing or modifying archives (read only).

  Like components/files/oblivion/esm.{hpp,cpp} this is a self contained,
  validating reader. Extracted contents are cached in memory; the cache is
  bounded (see setCacheLimit) and shared by every consumer of this object,
  including the engine's VFS (components/vfs/bsaarchive.hpp), so repeated opens
  of the same archived file (e.g. on every SetTexture on an already-loaded
  texture) do not hit the disk or the decompressor again.

  Thread safety: open()/close() must not race with anything. getFile()/find()
  may be called concurrently from any number of threads; the internal cache is
  synchronised. Two threads racing to extract the same file may both do the
  work, but they will observe the same bytes and the cache is never corrupted.

  Loading order and precedence: BSAs are registered with the VFS in the order
  given by the content configuration (for Oblivion, the order of the archives
  in the load order); later archives take precedence over earlier ones and
  loose files take precedence over all BSAs. This class does not implement
  precedence itself - it is a pure container and the VFS manager
  (components/vfs/manager.cpp) decides which archive wins. Archive invalidation
  is not implemented; ordering is the only mechanism available.

  File names inside the archive use backslashes and are (conventionally)
  lower case. All lookups performed by this class canonicalise both the
  requested name and the stored names with normalizeName(), so '/' vs '\\' and
  letter case do not matter.
*/

#include <cstddef>
#include <cstdint>
#include <deque>
#include <filesystem>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

#include <components/bsa/bsafile.hpp> // Bsa::BSAFile::FileStruct, Bsa::BSAFile::FileList
#include <components/files/istreamptr.hpp>

namespace Files
{
    namespace Oblivion
    {
        // Archive flags as found in Header::mFlags. Numeric values match the
        // Bethesda documentation; some bits are only meaningful for later
        // games or carry no structural meaning for Oblivion.
        enum ArchiveFlag : std::uint32_t
        {
            ArchiveFlag_FolderNames = 0x0001, // archive has names for the folders
            ArchiveFlag_FileNames = 0x0002, // archive has names for the files
            ArchiveFlag_CompressedFiles = 0x0004, // files are compressed by default
            ArchiveFlag_RetainDirectoryNames = 0x0008, // no structural effect (memory hint)
            ArchiveFlag_RetainFileNames = 0x0010, // no structural effect (memory hint, set for sound archives)
            ArchiveFlag_RetainFileOffsets = 0x0020, // no structural effect ("retain file name offsets" in BSA v104)
            ArchiveFlag_Xbox360 = 0x0040, // big endian numbers and hashes; not supported, see open()
            ArchiveFlag_StartUpStrings = 0x0080, // no structural effect (related to INI invalidation settings)
            ArchiveFlag_Unknown100 = 0x0100, // set in all official Oblivion archives, no structural effect
                                               // ("embedded file names" in BSA v104)
            ArchiveFlag_Unknown200 = 0x0200, // set in all official Oblivion archives, no structural effect
                                               // (XMEM codec in BSA v105)
        };

        // File flags as found in Header::mFileFlags. These describe the
        // categories of files packed into the archive; Oblivion sets
        // FileFlag_Misc in basically every archive.
        enum FileFlag : std::uint32_t
        {
            FileFlag_Meshes = 0x0001, // .nif, .kf, .egm, .egt
            FileFlag_Textures = 0x0002, // .dds
            FileFlag_Menus = 0x0004, // .xml
            FileFlag_Sounds = 0x0008, // .wav
            FileFlag_Voices = 0x0010, // .mp3, .xwm
            FileFlag_Shaders = 0x0020, // .txt, .scc (shader configuration), .bat, .html
            FileFlag_Trees = 0x0040, // .spt
            FileFlag_Fonts = 0x0080, // .tex, .fnt
            FileFlag_Misc = 0x0100, // .ctl and everything else
            FileFlag_Lip = 0x0200, // .lip
        };

        // The high bits of a file record's size field.
        enum FileSizeFlag : std::uint32_t
        {
            FileSizeFlag_Compression = 0x40000000, // inverts the default compression of the archive for this file
        };

        // Mask applied to a file record's size field to get the size on disk.
        inline constexpr std::uint32_t FileSizeMask = 0x3fffffff;

        class BsaFile
        {
        public:
            /// The "BSA\0" magic of the archive, as found in Header::mFileId.
            static constexpr std::uint32_t ArchiveMagic = 0x00415342;

            /// The only archive version this class supports (103).
            static constexpr std::uint32_t ArchiveVersion = 0x67;

            /// Default budget for the extracted-file cache, in bytes (16 MiB).
            static constexpr std::size_t DefaultCacheLimit = 16 * 1024 * 1024;

            struct Header
            {
                std::uint32_t mFileId = 0; // ArchiveMagic
                std::uint32_t mVersion = 0; // ArchiveVersion
                std::uint32_t mFoldersOffset = 0; // absolute offset of the folder records
                std::uint32_t mFlags = 0; // see ArchiveFlag
                std::uint32_t mFolderCount = 0; // number of folder records
                std::uint32_t mFileCount = 0; // total number of files in all folders
                std::uint32_t mFolderNamesLength = 0; // total length of the folder name table
                std::uint32_t mFileNamesLength = 0; // total length of the file name table
                std::uint32_t mFileFlags = 0; // see FileFlag

                bool hasFlag(std::uint32_t flag) const { return (mFlags & flag) != 0; }

                bool hasFolderNames() const { return hasFlag(ArchiveFlag_FolderNames); }
                bool hasFileNames() const { return hasFlag(ArchiveFlag_FileNames); }
                bool hasDefaultCompression() const { return hasFlag(ArchiveFlag_CompressedFiles); }
                bool hasRetainDirectoryNames() const { return hasFlag(ArchiveFlag_RetainDirectoryNames); }
                bool hasRetainFileNames() const { return hasFlag(ArchiveFlag_RetainFileNames); }
                bool hasRetainFileOffsets() const { return hasFlag(ArchiveFlag_RetainFileOffsets); }
                bool hasStartUpStrings() const { return hasFlag(ArchiveFlag_StartUpStrings); }
                bool isXbox360() const { return hasFlag(ArchiveFlag_Xbox360); }
                bool hasFileFlag(FileFlag flag) const { return (mFileFlags & flag) != 0; }
            };

            static_assert(sizeof(Header) == 36, "Oblivion BSA header must be 36 bytes");

            /// One file within the archive. mName holds the stored path in
            /// the archive ('\\' separators, no trailing terminator); it is
            /// empty for archives that do not store file names.
            struct FileRecord
            {
                std::uint64_t mHash = 0;
                std::uint64_t mFolderHash = 0; // hash of the folder this record lives in (0 for the root folder)
                std::vector<char> mName;
                std::uint32_t mSize = 0; // raw size field, includes FileSizeFlag_Compression
                std::uint32_t mOffset = 0; // absolute offset of the file data within the archive

                /// Size of the file data on disk (after removing the flag bits).
                std::uint32_t sizeOnDisk() const { return mSize & FileSizeMask; }
                /// True when the flag bit is set on this file.
                bool hasCompressionFlag() const { return (mSize & FileSizeFlag_Compression) != 0; }
                /// True when this file is zlib compressed, honouring both the
                /// archive default and the per-file flag.
                bool isCompressed(bool archiveDefaultCompressed) const
                {
                    return hasCompressionFlag() != archiveDefaultCompressed;
                }
                /// Stored path with '\\' separators, e.g. "meshes\\architecture\\wall.nif".
                std::string name() const { return std::string(mName.begin(), mName.end()); }
            };

            struct FolderRecord
            {
                std::uint64_t mHash = 0;
                std::uint32_t mCount = 0;
                std::uint32_t mOffset = 0; // offset of the file record block; informational only
                std::string mName; // stored folder path, e.g. "meshes\\architecture"
                std::map<std::uint64_t, FileRecord> mFiles; // keyed by file hash
            };

            BsaFile() = default;
            explicit BsaFile(const std::filesystem::path& path) { open(path); }
            BsaFile(const BsaFile&) = delete;
            BsaFile& operator=(const BsaFile&) = delete;
            ~BsaFile();

            /// Opens the archive and parses its tables. Throws std::runtime_error
            /// on malformed files, wrong versions or Xbox 360 archives.
            void open(const std::filesystem::path& path);

            /// Drops all parsed data and cached extractions. Called by the destructor.
            void close();

            bool isOpen() const { return mOpen; }

            /// The parsed header. Valid after open(). Throws when not open.
            const Header& header() const;

            const std::filesystem::path& getPath() const { return mFilepath; }
            std::string getFilename() const;

            /// Flat list of all files, exposed for the VFS integration
            /// (components/vfs/bsaarchive.hpp) and compatible with
            /// Bsa::BSAFile::FileStruct. Files from archives without a
            /// file name table are omitted (they are not addressable by name).
            /// Sorted by name. Stable until close().
            const Bsa::BSAFile::FileList& getList() const { return mFiles; }

            /// Folders of the archive keyed by folder hash. Valid after open().
            const std::map<std::uint64_t, FolderRecord>& folders() const { return mFolders; }

            /// Finds a file by name. '/' and '\\' are equivalent, lookup is
            /// case insensitive. Returns nullptr when not present.
            const FileRecord* findFile(std::string_view name) const;

            /// Like findFile(), but also succeeds without a file name table
            /// (hash lookup). Returns the record of the given file hash, or nullptr.
            const FileRecord* findFileByHash(std::uint64_t folderHash, std::uint64_t fileHash) const;

            /// True when the archive contains the given file. See findFile().
            bool contains(std::string_view name) const { return findFile(name) != nullptr; }

            /// Reads and (when needed) decompresses the given file. Extracted
            /// contents are served from the cache when possible. The returned
            /// stream stays valid independently of this object. The record must
            /// come from this archive (e.g. through findFile()/folders()/getList()),
            /// otherwise std::runtime_error is thrown.
            Files::IStreamPtr getFile(const FileRecord& record) const;

            /// VFS glue: looks the record up from a Bsa::BSAFile::FileStruct
            /// previously obtained through getList().
            Files::IStreamPtr getFile(const Bsa::BSAFile::FileStruct* file) const;

            /// Sets the extraction cache budget in bytes. Passing 0 disables
            /// caching entirely. Shrinking the budget evicts the oldest
            /// entries immediately.
            void setCacheLimit(std::size_t bytes);
            std::size_t cacheLimit() const { return mCacheLimit; }
            std::size_t cacheSize() const
            {
                std::lock_guard lock(mCacheMutex);
                return mCacheBytes;
            }
            std::size_t cacheEntryCount() const
            {
                std::lock_guard lock(mCacheMutex);
                return mCache.size();
            }
            void clearCache();

            /// Canonicalises a file or folder name for hashing and comparison:
            /// '/' and '\\' become '\\', letters are lower cased (ASCII only),
            /// leading/trailing separators are dropped, repeated separators
            /// are collapsed.
            static std::string normalizeName(std::string_view name);

            /// The Oblivion/TES4 path hash, computed on the given stem and
            /// (optional) extension. '/' is treated as '\\'. Extensions must
            /// include the leading dot (e.g. ".nif") and both arguments are
            /// expected to be lower case, as produced by normalizeName().
            static std::uint64_t generateHash(std::string_view stem, std::string_view extension);

        private:
            [[noreturn]] void fail(const std::string& msg) const;

            void readHeader(std::istream& input);
            std::shared_ptr<const std::vector<char>> extract(const FileRecord& record) const;
            void insertIntoCache(const std::uint64_t key, std::shared_ptr<const std::vector<char>> data) const;

            std::filesystem::path mFilepath;
            bool mOpen = false;
            std::uint64_t mArchiveSize = 0; // total size of the archive in bytes
            Header mHeader{};
            std::map<std::uint64_t, FolderRecord> mFolders;
            Bsa::BSAFile::FileList mFiles;
            std::unordered_map<const Bsa::BSAFile::FileStruct*, const FileRecord*> mFileStructMap;

            mutable std::mutex mCacheMutex;
            mutable std::unordered_map<std::uint64_t, std::shared_ptr<const std::vector<char>>> mCache;
            mutable std::deque<std::uint64_t> mCacheOrder; // FIFO eviction order
            std::size_t mCacheLimit = DefaultCacheLimit;
            mutable std::size_t mCacheBytes = 0;
        };
    } // namespace Oblivion
} // namespace Files

#endif
