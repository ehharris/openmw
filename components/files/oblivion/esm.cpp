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
#include "esm.hpp"

#include <cstring>
#include <istream>
#include <span>
#include <stdexcept>
#include <vector>
#include <memory>

#include <zlib.h>

#include <fstream>

#include <components/files/conversion.hpp>
#include <components/files/openfile.hpp>
#include <components/files/utils.hpp>

namespace Files
{
    namespace Oblivion
    {
        namespace
        {
            constexpr std::uint32_t RECORD_GRUP = ESM::fourCC("GRUP");

            // Sanity limit for a single record's on-disk data size (4 GiB minus a byte).
            constexpr std::uint32_t MAX_RECORD_SIZE = 0xfffffffe;

            // The TES4 record of Oblivion.esm is a few tens of KiB; plugins are far smaller.
            // Cap it generously to avoid huge allocations on corrupt files.
            constexpr std::uint32_t MAX_HEADER_RECORD_SIZE = 16 * 1024 * 1024; // 16 MiB

            // Arbitrary deep guard against corrupt files with absurd group nesting.
            constexpr std::size_t MAX_GROUP_DEPTH = 32;

            // Adds the given filename, if any, as a prefix for error messages.
            std::string errorContext(const std::filesystem::path& filename, std::string_view context)
            {
                if (filename.empty())
                    return std::string(context);
                return Files::pathToUnicodeString(filename) + ": " + std::string(context);
            }

            std::uint32_t streamSizeLeft(std::istream& stream)
            {
                const std::streamsize left = Files::getStreamSizeLeft(stream);
                if (left < 0 || static_cast<std::uint64_t>(left) > MAX_RECORD_SIZE)
                    throw std::runtime_error("Oblivion plugin is too large");
                return static_cast<std::uint32_t>(left);
            }

            std::vector<char> readExact(std::istream& stream, std::uint32_t size, const std::filesystem::path& filename,
                std::string_view what)
            {
                std::vector<char> buffer(size);
                stream.read(buffer.data(), static_cast<std::streamsize>(size));
                if (!stream || stream.gcount() != static_cast<std::streamsize>(size))
                    throw std::runtime_error(errorContext(filename, "failed to read " + std::string(what)));
                return buffer;
            }

            // Parses the sub-records of a record payload. Handles XXXX subrecords that
            // redefine the size of the following subrecord.
            std::vector<SubRecord> parseSubRecords(
                std::span<const char> payload, const std::filesystem::path& filename, std::uint32_t recordType)
            {
                std::vector<SubRecord> subRecords;

                std::size_t pos = 0;
                while (pos < payload.size())
                {
                    if (payload.size() - pos < sizeof(SubRecordHeader))
                        throw std::runtime_error(errorContext(filename, "truncated subrecord header in record "
                            + ESM::printName(recordType)));

                    SubRecordHeader subHeader{};
                    std::memcpy(&subHeader, payload.data() + pos, sizeof(subHeader));
                    pos += sizeof(subHeader);

                    std::uint32_t dataSize = subHeader.dataSize;
                    if (subHeader.typeId == ESM::fourCC("XXXX"))
                    {
                        // Extended size for the next subrecord
                        if (dataSize != sizeof(std::uint32_t))
                            throw std::runtime_error(
                                errorContext(filename, "XXXX subrecord in " + ESM::printName(recordType)
                                    + " has size " + std::to_string(dataSize) + ", expected 4"));
                        if (payload.size() - pos < sizeof(std::uint32_t))
                            throw std::runtime_error(errorContext(
                                filename, "truncated XXXX subrecord in record " + ESM::printName(recordType)));

                        std::memcpy(&dataSize, payload.data() + pos, sizeof(dataSize));
                        pos += sizeof(dataSize);

                        if (payload.size() - pos < sizeof(SubRecordHeader))
                            throw std::runtime_error(errorContext(filename, "truncated subrecord header after XXXX in "
                                + ESM::printName(recordType)));

                        std::memcpy(&subHeader, payload.data() + pos, sizeof(subHeader));
                        pos += sizeof(subHeader);
                    }

                    if (payload.size() - pos < dataSize)
                        throw std::runtime_error(errorContext(filename, "subrecord " + ESM::printName(subHeader.typeId)
                            + " is larger than its containing record " + ESM::printName(recordType)));

                    SubRecord subRecord;
                    subRecord.type = subHeader.typeId;
                    subRecord.data.assign(payload.begin() + static_cast<std::ptrdiff_t>(pos),
                        payload.begin() + static_cast<std::ptrdiff_t>(pos + dataSize));
                    pos += dataSize;
                    subRecords.push_back(std::move(subRecord));
                }

                return subRecords;
            }

            // Decompresses a zlib stream. On failure the reason is included in the exception.
            std::vector<char> decompress(const std::vector<char>& compressed, std::uint32_t uncompressedSize,
                const std::filesystem::path& filename, std::uint32_t recordType)
            {
                std::vector<char> decompressed(uncompressedSize);

                z_stream stream{};
                stream.next_in = reinterpret_cast<Bytef*>(const_cast<char*>(compressed.data()));
                stream.avail_in = static_cast<uInt>(compressed.size());
                stream.next_out = reinterpret_cast<Bytef*>(decompressed.data());
                stream.avail_out = static_cast<uInt>(decompressed.size());

                if (const int ec = inflateInit(&stream); ec != Z_OK)
                    throw std::runtime_error(errorContext(filename, "failed to initialise zlib for record "
                        + ESM::printName(recordType) + ": error " + std::to_string(ec)));

                const int ec = inflate(&stream, Z_FINISH);
                inflateEnd(&stream);

                if (ec != Z_STREAM_END)
                    throw std::runtime_error(errorContext(filename, "failed to decompress record "
                        + ESM::printName(recordType) + ": error " + std::to_string(ec)));
                if (stream.total_out != uncompressedSize)
                    throw std::runtime_error(errorContext(filename, "decompressed record "
                        + ESM::printName(recordType) + " has size " + std::to_string(stream.total_out) + ", expected "
                        + std::to_string(uncompressedSize)));

                return decompressed;
            }

            // Reads the payload of a record, transparently handling zlib-compressed records.
            // Returns the (decompressed) payload.
            std::vector<char> readRecordPayload(const RecordTypeHeader& header, std::istream& stream,
                const std::filesystem::path& filename, std::uint64_t bytesLeft)
            {
                if (header.dataSize > bytesLeft)
                    throw std::runtime_error(errorContext(filename, "record " + ESM::printName(header.typeId)
                        + " is larger than the remaining file"));

                if (!isCompressedRecord(header.flags))
                    return readExact(stream, header.dataSize, filename, "record " + ESM::printName(header.typeId));

                // Compressed records start with the uncompressed size as a little-endian uint32
                if (header.dataSize < sizeof(std::uint32_t))
                    throw std::runtime_error(errorContext(filename, "compressed record "
                        + ESM::printName(header.typeId) + " is too small"));

                std::uint32_t uncompressedSize = 0;
                stream.read(reinterpret_cast<char*>(&uncompressedSize), sizeof(uncompressedSize));
                if (!stream || stream.gcount() != static_cast<std::streamsize>(sizeof(uncompressedSize)))
                    throw std::runtime_error(errorContext(filename, "failed to read the uncompressed size of record "
                        + ESM::printName(header.typeId)));

                const std::uint32_t compressedSize = header.dataSize - sizeof(uncompressedSize);
                // zlib format theoretical maximum expansion
                if (static_cast<std::uint64_t>(uncompressedSize) > static_cast<std::uint64_t>(compressedSize) * 1032)
                    throw std::runtime_error(errorContext(filename, "compressed record "
                        + ESM::printName(header.typeId) + " expands to an invalid size"));

                std::vector<char> compressed = readExact(
                    stream, compressedSize, filename, "compressed data of record " + ESM::printName(header.typeId));

                return decompress(compressed, uncompressedSize, filename, header.typeId);
            }
        }

        bool isKnownGroupType(std::int32_t type)
        {
            switch (type)
            {
                case Grp_RecordType:
                case Grp_WorldChild:
                case Grp_InteriorCellBlock:
                case Grp_InteriorCellSubBlock:
                case Grp_ExteriorCellBlock:
                case Grp_ExteriorCellSubBlock:
                case Grp_CellChild:
                case Grp_TopicChild:
                case Grp_CellPersistentChild:
                case Grp_CellTemporaryChild:
                case Grp_CellVisibleDistantChild:
                    return true;
                default:
                    return false;
            }
        }

        bool isOblivionRecordType(std::uint32_t typeId)
        {
            switch (typeId)
            {
                case REC_ACHR:
                case REC_ACRE:
                case REC_ACTI:
                case REC_ADDN:
                case REC_ALCH:
                case REC_AMMO:
                case REC_ANIO:
                case REC_APPA:
                case REC_ARMA:
                case REC_ARMO:
                case REC_ARTI:
                case REC_BOOK:
                case REC_BSGN:
                case REC_CELL:
                case REC_CLAS:
                case REC_CLMT:
                case REC_CLOT:
                case REC_CONT:
                case REC_CREA:
                case REC_CSTY:
                case REC_DIAL:
                case REC_DOOR:
                case REC_ECZN:
                case REC_EFSH:
                case REC_ENCH:
                case REC_EYES:
                case REC_FACT:
                case REC_FLOR:
                case REC_FURN:
                case REC_GLOB:
                case REC_GMST:
                case REC_GRAS:
                case REC_GRUP:
                case REC_HAIR:
                case REC_IDLE:
                case REC_IMAD:
                case REC_IMGS:
                case REC_IDLM:
                case REC_INGR:
                case REC_KEYM:
                case REC_LAND:
                case REC_LGTM:
                case REC_LIGH:
                case REC_LTEX:
                case REC_LVLC:
                case REC_LVLI:
                case REC_LVLN:
                case REC_LVSP:
                case REC_MGEF:
                case REC_MISC:
                case REC_MOVT:
                case REC_MSTT:
                case REC_MUSC:
                case REC_MUST:
                case REC_NOTE:
                case REC_NPC_:
                case REC_OTFT:
                case REC_PACK:
                case REC_PGRD:
                case REC_QUST:
                case REC_RACE:
                case REC_REFR:
                case REC_REGN:
                case REC_ROAD:
                case REC_SBSP:
                case REC_SCPT:
                case REC_SCRL:
                case REC_SGST:
                case REC_SKIL:
                case REC_SLGM:
                case REC_SOUN:
                case REC_SPEL:
                case REC_STAT:
                case REC_TACT:
                case REC_TES4:
                case REC_TREE:
                case REC_WATR:
                case REC_WEAP:
                case REC_WRLD:
                case REC_WTHR:
                    return true;
                default:
                    return false;
            }
        }

        bool isOblivionHedrVersion(std::uint32_t version)
        {
            // Oblivion loads plugins with HEDR versions 0.8 and 1.0
            return version == ESM::VER_080 || version == ESM::VER_100;
        }

        std::string SubRecord::asZString() const
        {
            std::string result(data.begin(), data.end());
            const std::size_t nul = result.find('\0');
            if (nul != std::string::npos)
                result.erase(nul);
            return result;
        }

        const SubRecord* Record::findSubRecord(std::uint32_t type) const
        {
            for (const SubRecord& sub : subRecords)
                if (sub.type == type)
                    return &sub;
            return nullptr;
        }

        std::vector<const SubRecord*> Record::findSubRecords(std::uint32_t type) const
        {
            std::vector<const SubRecord*> result;
            for (const SubRecord& sub : subRecords)
                if (sub.type == type)
                    result.push_back(&sub);
            return result;
        }

        EsmFile::EsmFile() = default;

        EsmFile::EsmFile(const std::filesystem::path& path)
        {
            open(path);
        }

        EsmFile::~EsmFile() = default;

        void EsmFile::open(const std::filesystem::path& path)
        {
            close();

            Files::IStreamPtr stream = Files::openBinaryInputFileStream(path);
            open(std::move(stream), path);
        }

        void EsmFile::open(Files::IStreamPtr&& stream, const std::filesystem::path& filename)
        {
            close();

            if (!stream)
                throw std::runtime_error("Oblivion plugin stream is not valid");

            mFilename = filename;
            mStream = std::move(stream);

            // The stream might not be at the beginning; the size is measured from here
            try
            {
                readHeaderRecord();
            }
            catch (...)
            {
                close();
                throw;
            }

            mOpen = true;
        }

        void EsmFile::close()
        {
            mStream.reset();
            mHeader = Header{};
            mRecords.clear();
            mGroups.clear();
            mOpen = false;
            mParsed = false;
        }

        void EsmFile::readHeaderRecord()
        {
            if (!mStream)
                throw std::runtime_error("Oblivion plugin is not open");

            const std::uint32_t sizeLeft = streamSizeLeft(*mStream);
            if (sizeLeft < sizeof(RecordTypeHeader))
                throw std::runtime_error(
                    errorContext(mFilename, "file is too small to contain a TES4 header record"));

            RecordTypeHeader header{};
            mStream->read(reinterpret_cast<char*>(&header), sizeof(header));
            if (!mStream || mStream->gcount() != static_cast<std::streamsize>(sizeof(header)))
                throw std::runtime_error(errorContext(mFilename, "failed to read the TES4 header record"));

            if (header.typeId != REC_TES4)
                throw std::runtime_error(errorContext(mFilename,
                    "file is not an Oblivion plugin: expected a TES4 record, got " + ESM::printName(header.typeId)));
            if (header.dataSize > MAX_HEADER_RECORD_SIZE)
                throw std::runtime_error(errorContext(mFilename, "TES4 header record is too large ("
                    + std::to_string(header.dataSize) + " bytes)"));

            const std::uint32_t payloadLeft = streamSizeLeft(*mStream);
            std::vector<char> payload = readRecordPayload(header, *mStream, mFilename, payloadLeft);

            Header result;
            result.flags = header.flags;

            for (const SubRecord& sub : parseSubRecords(payload, mFilename, REC_TES4))
            {
                switch (sub.type)
                {
                    case ESM::fourCC("HEDR"):
                    {
                        if (sub.data.size() != sizeof(Data))
                            throw std::runtime_error(errorContext(mFilename, "HEDR has size "
                                + std::to_string(sub.data.size()) + ", expected " + std::to_string(sizeof(Data))));
                        std::memcpy(&result.mData, sub.data.data(), sizeof(result.mData));
                        break;
                    }
                    case ESM::fourCC("CNAM"):
                        result.mAuthor = sub.asZString();
                        break;
                    case ESM::fourCC("SNAM"):
                        result.mDesc = sub.asZString();
                        break;
                    case ESM::fourCC("MAST"):
                    {
                        ESM::MasterData master;
                        master.name = sub.asZString();
                        master.size = 0;
                        result.mMaster.push_back(std::move(master));
                        break;
                    }
                    case ESM::fourCC("DATA"):
                    {
                        if (result.mMaster.empty())
                            throw std::runtime_error(errorContext(mFilename,
                                "DATA subrecord without a preceding MAST subrecord"));
                        if (sub.data.size() != sizeof(std::uint64_t))
                            throw std::runtime_error(errorContext(mFilename, "DATA has size "
                                + std::to_string(sub.data.size()) + ", expected 8"));
                        std::memcpy(&result.mMaster.back().size, sub.data.data(), sizeof(std::uint64_t));
                        break;
                    }
                    case ESM::fourCC("ONAM"):
                    {
                        if (sub.data.size() % sizeof(std::uint32_t) != 0)
                            throw std::runtime_error(errorContext(mFilename, "ONAM has size "
                                + std::to_string(sub.data.size()) + ", expected a multiple of 4"));
                        result.mOverrides.reserve(sub.data.size() / sizeof(std::uint32_t));
                        for (std::size_t i = 0; i < sub.data.size(); i += sizeof(std::uint32_t))
                        {
                            std::uint32_t value = 0;
                            std::memcpy(&value, sub.data.data() + i, sizeof(value));
                            result.mOverrides.push_back(value);
                        }
                        break;
                    }
                    // Vestigial/optional subrecords that Oblivion.esm contains
                    case ESM::fourCC("OFST"): // type offsets, vestigial
                    case ESM::fourCC("DELE"): // deleted form ids, only in Oblivion.esm
                    case ESM::fourCC("INTV"): // CK only
                    case ESM::fourCC("INCC"): // CK only
                    case ESM::fourCC("TNAM"): // Fallout 4
                    case ESM::fourCC("MMSB"): // Fallout 76
                        break;
                    default:
                        throw std::runtime_error(errorContext(mFilename, "unknown subrecord in TES4 record: "
                            + ESM::printName(sub.type)));
                }
            }

            mHeader = std::move(result);
        }

        void EsmFile::parse()
        {
            if (!mOpen)
                throw std::runtime_error("Oblivion plugin is not open");
            if (mParsed)
                return;

            struct GroupFrame
            {
                std::uint64_t remaining; // bytes left in the group, after the 20 byte header
                std::size_t index; // index into mGroups
            };

            std::vector<GroupFrame> stack;

            const auto subtractFromGroups = [&stack, this](std::uint64_t bytes) {
                for (GroupFrame& frame : stack)
                {
                    if (bytes > frame.remaining)
                        throw std::runtime_error(
                            errorContext(mFilename, "content extends past the end of its containing group"));
                    frame.remaining -= bytes;
                }
                while (!stack.empty() && stack.back().remaining == 0)
                    stack.pop_back();
            };

            std::istream& stream = *mStream;
            while (true)
            {
                char headerBytes[sizeof(RecordTypeHeader)]{};
                stream.read(headerBytes, sizeof(headerBytes));
                const std::streamsize bytesRead = stream.gcount();
                if (bytesRead == 0)
                    break; // end of file
                if (bytesRead != static_cast<std::streamsize>(sizeof(headerBytes)))
                    throw std::runtime_error(errorContext(mFilename, "truncated record header at the end of file"));
    
                std::uint32_t typeId = 0;
                std::memcpy(&typeId, headerBytes, sizeof(typeId));

                if (typeId == RECORD_GRUP)
                {
                    GroupHeader groupHeader{};
                    std::memcpy(&groupHeader, headerBytes, sizeof(groupHeader));

                    if (groupHeader.groupSize < sizeof(groupHeader))
                        throw std::runtime_error(
                            errorContext(mFilename, "group has size " + std::to_string(groupHeader.groupSize)
                                + ", smaller than its header"));
                    if (stack.size() >= MAX_GROUP_DEPTH)
                        throw std::runtime_error(errorContext(mFilename, "group nesting is too deep"));

                    Group group;
                    group.label = groupHeader.label;
                    group.type = groupHeader.type;
                    group.groupSize = groupHeader.groupSize;
                    group.versionControlInfo = groupHeader.stamp;
                    group.depth = stack.size();
                    mGroups.push_back(group);

                    // Charge the enclosing groups for the group header itself
                    subtractFromGroups(sizeof(groupHeader));

                    const std::uint64_t remaining = groupHeader.groupSize - sizeof(groupHeader);
                    // A zero-sized group consumes no further bytes; not pushing a frame for it avoids an endless loop
                    if (remaining > 0)
                        stack.push_back(GroupFrame{ remaining, mGroups.size() - 1 });
                    continue;
                }

                RecordTypeHeader recordHeader{};
                std::memcpy(&recordHeader, headerBytes, sizeof(recordHeader));

                const std::uint64_t bytesLeft = streamSizeLeft(stream);
                std::vector<char> payload = readRecordPayload(recordHeader, stream, mFilename, bytesLeft);

                Record record;
                record.typeId = recordHeader.typeId;
                record.flags = recordHeader.flags;
                record.formId = recordHeader.formId;
                record.versionControlInfo = recordHeader.versionControlInfo;
                record.subRecords = parseSubRecords(payload, mFilename, recordHeader.typeId);
                record.groupIndex = stack.empty() ? Record::noGroup : static_cast<std::uint32_t>(stack.back().index);
                mRecords.push_back(std::move(record));

                subtractFromGroups(sizeof(RecordTypeHeader) + recordHeader.dataSize);
            }

            if (!stack.empty())
                throw std::runtime_error(errorContext(mFilename, "end of file reached before the end of a group"));

            mParsed = true;
        }

        namespace
        {
            // Reads one magic effect (EFID/EFIT pair plus the optional SCIT/FULL that can
            // follow it). The current sub-record index is advanced past the effect.
            MagicEffect readMagicEffect(const std::vector<const SubRecord*>& subRecords, std::size_t& index,
                const std::filesystem::path& filename, std::uint32_t recordType)
            {
                const SubRecord* efid = subRecords[index];
                if (efid->type != ESM::fourCC("EFID"))
                    throw std::runtime_error(errorContext(filename, "expected an EFID subrecord in record "
                        + ESM::printName(recordType)));
                if (efid->data.size() != 4)
                    throw std::runtime_error(errorContext(filename, "EFID has size " + std::to_string(efid->data.size())
                        + ", expected 4"));

                std::uint32_t effectId = 0;
                std::memcpy(&effectId, efid->data.data(), sizeof(effectId));
                ++index;

                if (index >= subRecords.size() || subRecords[index]->type != ESM::fourCC("EFIT"))
                    throw std::runtime_error(errorContext(filename,
                        "EFID without a following EFIT in record " + ESM::printName(recordType)));

                const SubRecord* efit = subRecords[index];
                if (efit->data.size() < 24)
                    throw std::runtime_error(errorContext(filename, "EFIT has size " + std::to_string(efit->data.size())
                        + ", expected at least 24"));

                MagicEffect effect;
                std::memcpy(&effect.effect.effectId, efit->data.data(), 4);
                if (effect.effect.effectId != effectId)
                    throw std::runtime_error(errorContext(filename, "EFID and EFIT disagree on the effect id in record "
                        + ESM::printName(recordType)));
                std::memcpy(&effect.effect.magnitude, efit->data.data() + 4, 4);
                std::memcpy(&effect.effect.area, efit->data.data() + 8, 4);
                std::memcpy(&effect.effect.duration, efit->data.data() + 12, 4);
                std::memcpy(&effect.effect.deliveryType, efit->data.data() + 16, 4);
                std::memcpy(&effect.effect.actorValue, efit->data.data() + 20, 4);
                ++index;

                // Optional script effect data: SCIT followed by FULL
                if (index < subRecords.size() && subRecords[index]->type == ESM::fourCC("SCIT"))
                {
                    const SubRecord* scit = subRecords[index];
                    ScriptEffectItem item;
                    if (scit->data.size() >= 4)
                        std::memcpy(&item.scriptFormId, scit->data.data(), 4);
                    if (scit->data.size() >= 8)
                        std::memcpy(&item.school, scit->data.data() + 4, 4);
                    if (scit->data.size() >= 12)
                        std::memcpy(&item.visualEffectId, scit->data.data() + 8, 4);
                    if (scit->data.size() >= 16)
                        std::memcpy(&item.flags, scit->data.data() + 12, 4);
                    effect.script = item;
                    ++index;

                    if (index < subRecords.size() && subRecords[index]->type == ESM::fourCC("FULL"))
                    {
                        effect.scriptName = subRecords[index]->asZString();
                        ++index;
                    }
                }

                return effect;
            }

            std::vector<MagicEffect> readMagicEffects(const std::vector<const SubRecord*>& subRecords,
                std::size_t& index, const std::filesystem::path& filename, std::uint32_t recordType)
            {
                std::vector<MagicEffect> effects;
                while (index < subRecords.size())
                {
                    if (subRecords[index]->type != ESM::fourCC("EFID"))
                        break;
                    effects.push_back(readMagicEffect(subRecords, index, filename, recordType));
                }
                return effects;
            }

            // Subrecord iteration order: the spell/enchantment basics appear before the
            // effect records, the effects then appear as EFID/EFIT (optionally SCIT/FULL) groups.
            std::vector<const SubRecord*> collectSubRecords(const Record& record)
            {
                std::vector<const SubRecord*> order;
                order.reserve(record.subRecords.size());
                for (const SubRecord& sub : record.subRecords)
                    order.push_back(&sub);
                return order;
            }
        }

        Spell decodeSpell(const Record& record)
        {
            if (record.typeId != REC_SPEL)
                throw std::runtime_error(std::string("decodeSpell: expected a SPEL record, got ")
                    + ESM::printName(record.typeId));

            Spell spell;
            const std::vector<const SubRecord*> order = collectSubRecords(record);

            bool spitRead = false;
            for (std::size_t i = 0; i < order.size() && order[i]->type != ESM::fourCC("EFID"); ++i)
            {
                const SubRecord* sub = order[i];
                switch (sub->type)
                {
                    case ESM::fourCC("EDID"):
                        spell.editorId = sub->asZString();
                        break;
                    case ESM::fourCC("FULL"):
                        if (spell.name.empty())
                            spell.name = sub->asZString();
                        break;
                    case ESM::fourCC("SPIT"):
                        if (sub->data.size() != sizeof(SpellData))
                            throw std::runtime_error("decodeSpell: SPIT has size " + std::to_string(sub->data.size())
                                + ", expected " + std::to_string(sizeof(SpellData)));
                        std::memcpy(&spell.data, sub->data.data(), sizeof(spell.data));
                        spitRead = true;
                        break;
                    default:
                        break; // tolerate unknown subrecords
                }
            }

            if (!spitRead)
                throw std::runtime_error("decodeSpell: SPEL record without a SPIT subrecord");

            std::size_t effectsIndex = order.size();
            for (std::size_t i = 0; i < order.size(); ++i)
                if (order[i]->type == ESM::fourCC("EFID"))
                {
                    effectsIndex = i;
                    break;
                }

            spell.effects = readMagicEffects(order, effectsIndex, {}, REC_SPEL);
            return spell;
        }

        Enchantment decodeEnchantment(const Record& record)
        {
            if (record.typeId != REC_ENCH)
                throw std::runtime_error(std::string("decodeEnchantment: expected an ENCH record, got ")
                    + ESM::printName(record.typeId));

            Enchantment enchantment;
            const std::vector<const SubRecord*> order = collectSubRecords(record);

            bool enitRead = false;
            for (std::size_t i = 0; i < order.size() && order[i]->type != ESM::fourCC("EFID"); ++i)
            {
                const SubRecord* sub = order[i];
                switch (sub->type)
                {
                    case ESM::fourCC("EDID"):
                        enchantment.editorId = sub->asZString();
                        break;
                    case ESM::fourCC("FULL"):
                        if (enchantment.name.empty())
                            enchantment.name = sub->asZString();
                        break;
                    case ESM::fourCC("ENIT"):
                        if (sub->data.size() != sizeof(EnchantmentData))
                            throw std::runtime_error("decodeEnchantment: ENIT has size " + std::to_string(sub->data.size())
                                + ", expected " + std::to_string(sizeof(EnchantmentData)));
                        std::memcpy(&enchantment.data, sub->data.data(), sizeof(enchantment.data));
                        enitRead = true;
                        break;
                    default:
                        break; // tolerate unknown subrecords
                }
            }

            if (!enitRead)
                throw std::runtime_error("decodeEnchantment: ENCH record without an ENIT subrecord");

            std::size_t effectsIndex = order.size();
            for (std::size_t i = 0; i < order.size(); ++i)
                if (order[i]->type == ESM::fourCC("EFID"))
                {
                    effectsIndex = i;
                    break;
                }

            enchantment.effects = readMagicEffects(order, effectsIndex, {}, REC_ENCH);
            return enchantment;
        }
    }
}
