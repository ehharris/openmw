#ifndef OPENMW_COMPONENTS_OBLIVION_CONTENTLOADER_H
#define OPENMW_COMPONENTS_OBLIVION_CONTENTLOADER_H

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

/*
  Oblivion-specific content loading system.

  Oblivion plugins/masters (.esp/.esm) share the "TES4" signature and file
  layout with the other games of the TES4 family. This component implements
  the Oblivion flavour of content loading on top of the dedicated parser in
  components/files/oblivion/esm.{hpp,cpp}. It is used by tools, tests and by
  the engine's content loading pipeline (apps/openmw/mwworld/esmloader.cpp).

  Responsibilities:

    - validating Oblivion-specific game data files (TES4 signature and the
      Oblivion HEDR versions, see Files::Oblivion::isOblivionHedrVersion),
    - loading everything the engine needs to know about an Oblivion plugin
      before handing the file over to the streaming TES4-family reader
      (components/esm4) with a correct game format,
    - extracting the Oblivion-specific cell (CELL) and world space (WRLD)
      data, including the exterior-cell -> world space association that is
      implicit in the group hierarchy of the file,
    - tracking the master files a plugin depends on so that missing or
      incorrectly ordered dependencies can be reported (resource management:
      no dangling references can ever become visible),
    - collecting per-record-type statistics for diagnostics and progress
      reporting.

  This component intentionally does not replace the TES4-family streaming
  reader (components/esm4); the engine loads the records it needs through
  ESMStore::loadESM4() as usual. The classes here give the pipeline an
  Oblivion-aware view of the content before/while that happens.
*/

#include <cstdint>
#include <filesystem>
#include <string>
#include <unordered_map>
#include <vector>

#include <components/files/oblivion/esm.hpp>

namespace Files
{
    class Collections;
}

namespace Oblivion
{
    // Cell DATA flag: the cell is an interior cell (CELL_Interior in ESM4 terms).
    inline constexpr std::uint16_t kCellFlagInterior = 0x0001;

    // World DATA flag: the world space is an Oblivion worldspace (WLD_Oblivion in ESM4 terms).
    inline constexpr std::uint8_t kWorldFlagOblivion = 0x04;

    // Sentinel used for a cell's water height when the XCLW subrecord is absent.
    inline constexpr float kInvalidWaterHeight = -200000.f;

    // One master dependency of an Oblivion plugin (a MAST/DATA pair in the TES4 record).
    struct MasterFile
    {
        std::string mName;
        std::uint64_t mSize = 0; // size in bytes, as recorded in the DATA subrecord
    };

    // A world space (WRLD record) found in an Oblivion plugin.
    struct WorldData
    {
        std::uint32_t mFormId = 0;
        std::uint32_t mFlags = 0; // record flags, see Files::Oblivion::RecordFlag
        std::uint8_t mWorldFlags = 0; // DATA subrecord, see kWorldFlagOblivion
        std::uint32_t mParent = 0; // WNAM: formId of the parent world space (0 for top-level worlds)
        std::uint32_t mClimate = 0; // CNAM: formId of the climate (0 if absent)
        std::uint32_t mWater = 0; // NAM2: formId of the water form (0 if absent)
        std::string mEditorId; // EDID
        std::string mFullName; // FULL
        std::string mMapFile; // ICON (TES4 only): path to the map bitmap, empty otherwise
        float mLandLevel = 0.f; // DNAM: default land height (0 if absent)
        float mWaterLevel = 0.f; // DNAM: default water height (0 if absent)

        // FormIds of the exterior cells belonging to this world space, in file order.
        std::vector<std::uint32_t> mCells;
    };

    // A cell (CELL record) found in an Oblivion plugin.
    struct CellData
    {
        std::uint32_t mFormId = 0;
        std::uint32_t mWorld = 0; // enclosing world space, 0 for interior cells (and cells written without groups)
        std::uint16_t mCellFlags = 0; // DATA subrecord, see kCellFlagInterior
        std::int32_t mX = 0; // XCLC: grid coordinates (exterior cells only)
        std::int32_t mY = 0;
        float mWaterHeight = kInvalidWaterHeight; // XCLW
        std::string mEditorId; // EDID
        std::string mFullName; // FULL

        bool isExterior() const { return (mCellFlags & kCellFlagInterior) == 0; }
    };

    // An Oblivion game data file (.esm or .esp) loaded into memory.
    //
    // Records themselves are not kept; instead this carries the header summary,
    // per-record-type statistics and the cell/world data that the content
    // loading pipeline needs. The lifetime of the loaded file is managed by the
    // caller through this value-owned object (RAII, no global state).
    struct ContentFile
    {
        std::filesystem::path mFilename;
        std::uint32_t mHedrVersion = 0; // HEDR version bits (VER_080 for 0.8, VER_100 for 1.0)
        bool mMaster = false; // Rec_ESM flag on the TES4 record (determined by the flag, not the extension)
        std::int32_t mRecordCount = 0; // HEDR record counter (records and groups, not counting the TES4 record)
        std::uint32_t mNextObjectId = 0; // HEDR next available object id
        std::string mAuthor; // CNAM
        std::string mDescription; // SNAM
        std::vector<MasterFile> mMasters; // MAST/DATA pairs, in order

        // Number of records of each type (keyed by fourCC, see Files::Oblivion::RecordTypes),
        // and the total number of records (groups not included).
        std::unordered_map<std::uint32_t, std::size_t> mRecordTypeCounts;

        std::vector<WorldData> mWorlds; // in file order
        std::vector<CellData> mCells; // in file order

        // Look up the number of records of the given (fourCC) type. Returns 0 for absent types.
        std::size_t getRecordCount(std::uint32_t typeId) const;

        // Total number of records (groups not included).
        std::size_t getTotalRecordCount() const;

        const WorldData* findWorld(std::uint32_t formId) const;
        const CellData* findCell(std::uint32_t formId) const;

        // All cells whose parent world space is the given world formId, in file order.
        std::vector<const CellData*> getCells(std::uint32_t worldFormId) const;
    };

    // Oblivion-specific content loading system.
    //
    // All member functions are stateless; a ContentLoader value can be shared,
    // moved or recreated freely. Loading a *full* plugin is potentially
    // expensive (the entire file is parsed and its subrecords decoded); use
    // validate() when only the header is needed.
    class ContentLoader
    {
    public:
        // Cheap check whether the file at the given path is an Oblivion plugin or
        // master: TES4 signature and an HEDR version recognised by
        // Files::Oblivion::isOblivionHedrVersion. Does not parse the rest of the
        // file. Returns false on any error.
        static bool isOblivionPlugin(const std::filesystem::path& filepath) noexcept;

        // Validates the TES4 header of the given file. Throws std::runtime_error
        // if the file is not a valid Oblivion plugin (wrong signature, truncated
        // or unsupported header). Returns the parsed header on success.
        static Files::Oblivion::Header validate(const std::filesystem::path& filepath);

        // Loads and fully parses the given Oblivion game data file.
        // Throws std::runtime_error if the file is not a valid Oblivion plugin.
        ContentFile load(const std::filesystem::path& filepath) const;

        // Names of the plugin's master dependencies that are not available in
        // the given collections. Compared case-insensitively (like the load
        // order validation in the engine). Empty if every master is available.
        static std::vector<std::string> findMissingMasters(
            const ContentFile& content, const Files::Collections& collections);

        // Same, with the available file names given explicitly (case-insensitive).
        static std::vector<std::string> findMissingMasters(
            const ContentFile& content, const std::vector<std::string>& availableFiles);
    };

} /* namespace Oblivion */

#endif /* OPENMW_COMPONENTS_OBLIVION_CONTENTLOADER_H */
