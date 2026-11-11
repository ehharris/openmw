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

#include "contentloader.hpp"

#include <cstring>
#include <set>
#include <stdexcept>
#include <string>

#include <components/esm/common.hpp>
#include <components/esm/fourcc.hpp>
#include <components/files/collections.hpp>
#include <components/files/conversion.hpp>
#include <components/misc/strings/lower.hpp>

namespace Oblivion
{
    namespace
    {
        constexpr std::uint32_t NO_GROUP = Files::Oblivion::Record::noGroup;

        // Computes, for every group in the file, the index of its parent group
        // (noGroup for top-level groups) in a single pass. Groups appear in file
        // order and a group's parent is always pushed before it, so a stack of
        // open groups is sufficient.
        std::vector<std::uint32_t> computeParentGroups(const std::vector<Files::Oblivion::Group>& groups)
        {
            std::vector<std::uint32_t> parent(groups.size(), NO_GROUP);
            std::vector<std::uint32_t> stack;
            stack.reserve(groups.size() / 4 + 1);
            for (std::uint32_t i = 0; i < groups.size(); ++i)
            {
                const std::size_t depth = groups[i].depth;
                while (!stack.empty() && groups[stack.back()].depth >= depth)
                    stack.pop_back();
                parent[i] = stack.empty() ? NO_GROUP : stack.back();
                stack.push_back(i);
            }
            return parent;
        }

        // Index of the nearest enclosing group of the given type (Grp_*), or NO_GROUP.
        std::uint32_t findAncestorGroup(const std::vector<Files::Oblivion::Group>& groups,
            const std::vector<std::uint32_t>& parent, std::uint32_t groupIndex, std::int32_t type)
        {
            std::uint32_t current = groupIndex;
            while (current != NO_GROUP)
            {
                if (groups[current].type == type)
                    return current;
                current = parent[current];
            }
            return NO_GROUP;
        }

        std::uint32_t readU32(const Files::Oblivion::SubRecord& sub)
        {
            if (sub.data.size() != sizeof(std::uint32_t))
                throw std::runtime_error("Oblivion subrecord " + ESM::printName(sub.type) + " has size "
                    + std::to_string(sub.data.size()) + ", expected 4");
            std::uint32_t value = 0;
            std::memcpy(&value, sub.data.data(), sizeof(value));
            return value;
        }

        float readFloat32(const Files::Oblivion::SubRecord& sub)
        {
            if (sub.data.size() != sizeof(float))
                throw std::runtime_error("Oblivion subrecord " + ESM::printName(sub.type) + " has size "
                    + std::to_string(sub.data.size()) + ", expected 4");
            float value = 0.f;
            std::memcpy(&value, sub.data.data(), sizeof(value));
            return value;
        }

        CellData loadCell(const Files::Oblivion::Record& record)
        {
            CellData cell;
            cell.mFormId = record.formId;

            for (const Files::Oblivion::SubRecord& sub : record.subRecords)
            {
                switch (sub.type)
                {
                    case ESM::fourCC("EDID"):
                        cell.mEditorId = sub.asZString();
                        break;
                    case ESM::fourCC("FULL"):
                        cell.mFullName = sub.asZString();
                        break;
                    case ESM::fourCC("DATA"):
                        if (sub.data.size() == 1)
                            cell.mCellFlags = static_cast<std::uint16_t>(
                                static_cast<unsigned char>(sub.data[0]));
                        else if (sub.data.size() == sizeof(std::uint16_t))
                        {
                            std::uint16_t value = 0;
                            std::memcpy(&value, sub.data.data(), sizeof(value));
                            cell.mCellFlags = value;
                        }
                        else
                            throw std::runtime_error("CELL DATA subrecord has size "
                                + std::to_string(sub.data.size()) + ", expected 1 or 2");
                        break;
                    case ESM::fourCC("XCLC"):
                        if (sub.data.size() < 8)
                            throw std::runtime_error("CELL XCLC subrecord has size "
                                + std::to_string(sub.data.size()) + ", expected at least 8");
                        std::memcpy(&cell.mX, sub.data.data(), sizeof(cell.mX));
                        std::memcpy(&cell.mY, sub.data.data() + sizeof(cell.mX), sizeof(cell.mY));
                        break;
                    case ESM::fourCC("XCLW"):
                        cell.mWaterHeight = readFloat32(sub);
                        break;
                    default:
                        break; // not of interest for content loading
                }
            }
            return cell;
        }

        WorldData loadWorld(const Files::Oblivion::Record& record)
        {
            WorldData world;
            world.mFormId = record.formId;
            world.mFlags = record.flags;

            for (const Files::Oblivion::SubRecord& sub : record.subRecords)
            {
                switch (sub.type)
                {
                    case ESM::fourCC("EDID"):
                        world.mEditorId = sub.asZString();
                        break;
                    case ESM::fourCC("FULL"):
                        world.mFullName = sub.asZString();
                        break;
                    case ESM::fourCC("WNAM"): // parent world space (child worlds)
                        world.mParent = readU32(sub);
                        break;
                    case ESM::fourCC("ICON"): // map bitmap (TES4 only)
                        world.mMapFile = sub.asZString();
                        break;
                    case ESM::fourCC("CNAM"): // climate
                        world.mClimate = readU32(sub);
                        break;
                    case ESM::fourCC("NAM2"): // water form
                        world.mWater = readU32(sub);
                        break;
                    case ESM::fourCC("DATA"): // world flags, a single byte in TES4
                        if (!sub.data.empty())
                            world.mWorldFlags = static_cast<std::uint8_t>(
                                static_cast<unsigned char>(sub.data[0]));
                        break;
                    case ESM::fourCC("DNAM"): // default land/water levels, two floats
                        if (sub.data.size() == 8)
                        {
                            std::memcpy(&world.mLandLevel, sub.data.data(), sizeof(float));
                            std::memcpy(&world.mWaterLevel, sub.data.data() + sizeof(float), sizeof(float));
                        }
                        break;
                    default:
                        break; // not of interest for content loading
                }
            }
            return world;
        }

        std::string pathContext(const std::filesystem::path& path)
        {
            return Files::pathToUnicodeString(path);
        }
    } // namespace

    std::size_t ContentFile::getRecordCount(std::uint32_t typeId) const
    {
        const auto it = mRecordTypeCounts.find(typeId);
        return it != mRecordTypeCounts.end() ? it->second : 0;
    }

    std::size_t ContentFile::getTotalRecordCount() const
    {
        std::size_t total = 0;
        for (const auto& [typeId, count] : mRecordTypeCounts)
            total += count;
        return total;
    }

    const WorldData* ContentFile::findWorld(std::uint32_t formId) const
    {
        for (const WorldData& world : mWorlds)
            if (world.mFormId == formId)
                return &world;
        return nullptr;
    }

    const CellData* ContentFile::findCell(std::uint32_t formId) const
    {
        for (const CellData& cell : mCells)
            if (cell.mFormId == formId)
                return &cell;
        return nullptr;
    }

    std::vector<const CellData*> ContentFile::getCells(std::uint32_t worldFormId) const
    {
        std::vector<const CellData*> result;
        for (const CellData& cell : mCells)
            if (cell.mWorld == worldFormId)
                result.push_back(&cell);
        return result;
    }

    bool ContentLoader::isOblivionPlugin(const std::filesystem::path& filepath) noexcept
    {
        try
        {
            Files::Oblivion::EsmFile plugin(filepath);
            return Files::Oblivion::isOblivionHedrVersion(plugin.header().mData.version.ui);
        }
        catch (...)
        {
            return false;
        }
    }

    Files::Oblivion::Header ContentLoader::validate(const std::filesystem::path& filepath)
    {
        Files::Oblivion::EsmFile plugin(filepath);
        const Files::Oblivion::Header& header = plugin.header();
        if (!Files::Oblivion::isOblivionHedrVersion(header.mData.version.ui))
            throw std::runtime_error(pathContext(filepath)
                + ": not an Oblivion plugin/master (HEDR version " + std::to_string(header.mData.version.f) + ")");
        return header;
    }

    ContentFile ContentLoader::load(const std::filesystem::path& filepath) const
    {
        Files::Oblivion::EsmFile plugin(filepath);

        const Files::Oblivion::Header& header = plugin.header();
        if (!Files::Oblivion::isOblivionHedrVersion(header.mData.version.ui))
            throw std::runtime_error(pathContext(filepath)
                + ": not an Oblivion plugin/master (HEDR version " + std::to_string(header.mData.version.f) + ")");

        plugin.parse();

        ContentFile content;
        content.mFilename = filepath;
        content.mHedrVersion = header.mData.version.ui;
        content.mMaster = header.isMaster();
        content.mRecordCount = header.mData.records;
        content.mNextObjectId = header.mData.nextObjectId;
        content.mAuthor = header.mAuthor;
        content.mDescription = header.mDesc;
        content.mMasters.reserve(header.mMaster.size());
        for (const ESM::MasterData& master : header.mMaster)
            content.mMasters.push_back(MasterFile{ master.name, master.size });

        const std::vector<Files::Oblivion::Group>& groups = plugin.groups();
        const std::vector<std::uint32_t> parentGroups = computeParentGroups(groups);
        const auto worldGroupType = static_cast<std::int32_t>(Files::Oblivion::Grp_WorldChild);

        for (const Files::Oblivion::Record& record : plugin.records())
        {
            ++content.mRecordTypeCounts[record.typeId];

            if (record.typeId == Files::Oblivion::REC_WRLD)
                content.mWorlds.push_back(loadWorld(record));
            else if (record.typeId == Files::Oblivion::REC_CELL)
            {
                CellData cell = loadCell(record);
                if (cell.isExterior() && record.groupIndex != NO_GROUP)
                {
                    // Exterior cells live in a group hierarchy under a
                    // GRUP WorldChild whose label is the world space formId.
                    const std::uint32_t worldGroup = findAncestorGroup(groups, parentGroups, record.groupIndex, worldGroupType);
                    if (worldGroup != NO_GROUP)
                        cell.mWorld = groups[worldGroup].label;
                }
                content.mCells.push_back(std::move(cell));
            }
        }

        // Associate exterior cells with their world spaces (order-insensitive).
        std::unordered_map<std::uint32_t, std::size_t> worldByFormId;
        for (std::size_t i = 0; i < content.mWorlds.size(); ++i)
            worldByFormId.emplace(content.mWorlds[i].mFormId, i);
        for (const CellData& cell : content.mCells)
        {
            if (cell.mWorld == 0)
                continue;
            const auto it = worldByFormId.find(cell.mWorld);
            if (it != worldByFormId.end())
                content.mWorlds[it->second].mCells.push_back(cell.mFormId);
        }

        return content;
    }

    std::vector<std::string> ContentLoader::findMissingMasters(
        const ContentFile& content, const Files::Collections& collections)
    {
        std::vector<std::string> missing;
        for (const MasterFile& master : content.mMasters)
            if (!collections.doesExist(master.mName))
                missing.push_back(master.mName);
        return missing;
    }

    std::vector<std::string> ContentLoader::findMissingMasters(
        const ContentFile& content, const std::vector<std::string>& availableFiles)
    {
        std::set<std::string> available;
        for (const std::string& name : availableFiles)
            available.insert(Misc::StringUtils::lowerCase(name));

        std::vector<std::string> missing;
        for (const MasterFile& master : content.mMasters)
            if (!available.contains(Misc::StringUtils::lowerCase(master.mName)))
                missing.push_back(master.mName);
        return missing;
    }

} /* namespace Oblivion */
