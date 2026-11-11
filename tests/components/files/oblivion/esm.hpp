#ifndef OPENMW_COMPONENTS_FILES_OBLIVION_ESM_H
#define OPENMW_COMPONENTS_FILES_OBLIVION_ESM_H

/*
  Dedicated parser for Oblivion (TES4) ESM/ESP plugins.

  Oblivion plugins share the "TES4" signature and most of the file structure with the
  other TES4-family games (Fallout 3, Skyrim, Fallout 4, ...). This parser implements
  the Oblivion-specific flavour of the format:

    - 20 byte record headers (type, size, flags, formId, version control info), the
      form version fields of newer games are not present,
    - 20 byte group headers,
    - the Oblivion HEDR versions (0.8 and 1.0, see isOblivionHedrVersion),
    - Oblivion-specific record types and flags,
    - typed decoders for Oblivion-specific data structures (spell effects,
      enchantments, magic effect items, script effects),
    - zlib-compressed records (Rec_Compressed).

  Unlike the streaming engine loader (components/esm4) this parser materialises the
  whole file in memory. It is intended for tools, tests and validation, not as a drop
  in replacement for the engine's content loading path.

  Information about the structures is based on the Oblivion mod file format
  documentation (https://en.uesp.net/wiki/Oblivion_Mod:Mod_File_Format).
*/

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <iosfwd>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <type_traits>
#include <vector>

#include <components/esm/common.hpp> // ESM::ESMVersion, ESM::MasterData, ESM::printName
#include <components/esm/fourcc.hpp> // ESM::fourCC
#include <components/files/istreamptr.hpp>

namespace Files
{
    namespace Oblivion
    {
        // Record flags as found in the TES4 record header. Note that Oblivion does not
        // use Rec_Localized (that is a Skyrim+ flag), but it is tolerated when reading.
        enum RecordFlag : std::uint32_t
        {
            Rec_ESM = 0x00000001, // (TES4 record only) file is a master (.esm)
            Rec_Deleted = 0x00000020, // record was deleted by a later load order plugin
            Rec_Constant = 0x00000040,
            Rec_Localized = 0x00000080, // (TES4 record only, Skyrim+) localised string tables
            Rec_UpdateAnim = 0x00000100, // must update anims
            Rec_HiddenFromLocalMap = 0x00000200, // (REFR) hidden from the local map
            Rec_StartDead = 0x00000200, // (ACHR) starts dead
            Rec_Persistent = 0x00000400, // quest item / persistent reference
            Rec_Disabled = 0x00000800, // initially disabled
            Rec_Ignored = 0x00001000, // ignored by the game (e.g. in the CS ignoring list)
            Rec_VisDistant = 0x00008000, // visible when distant
            Rec_RandAnim = 0x00010000, // (ACTI) random animation start
            Rec_Danger = 0x00020000, // dangerous / off limits (interior cell)
            Rec_Compressed = 0x00040000, // record data is zlib compressed
            Rec_CanNotWait = 0x00080000, // the player can not wait here
            Rec_IgnoreObjInteract = 0x00100000, // (ACTI) ignore object interaction
            Rec_IsMarker = 0x00800000, // is a map/marker record
            Rec_Obstacle = 0x02000000, // (ACTI) obstacle / (REFR) no AI acquire
            Rec_MultiBound = 0x80000000, // (REFR) multibound
        };

        inline bool isCompressedRecord(std::uint32_t flags)
        {
            return (flags & Rec_Compressed) != 0;
        }

        // Group (GRUP) types. Oblivion only ever produces groups of these types,
        // but unknown values are tolerated on read (they are kept as-is) so that the
        // parser does not reject files produced by later/derived tools.
        enum GroupType : std::int32_t
        {
            Grp_RecordType = 0, // group of top-level records of a single type
            Grp_WorldChild = 1, // children of a world space
            Grp_InteriorCellBlock = 2, // block of interior cells
            Grp_InteriorCellSubBlock = 3,
            Grp_ExteriorCellBlock = 4, // block of exterior cells
            Grp_ExteriorCellSubBlock = 5,
            Grp_CellChild = 6, // children of a cell (REFR/ACHR/ACRE...)
            Grp_TopicChild = 7, // children of a DIAL topic (INFO)
            Grp_CellPersistentChild = 8, // persistent children of a cell
            Grp_CellTemporaryChild = 9, // temporary children of a cell
            Grp_CellVisibleDistantChild = 10, // visible-distant children of a cell
        };

        bool isKnownGroupType(std::int32_t type);

        // Record types of an Oblivion plugin. This is a superset of what Oblivion.esm
        // actually ships (it also lists types that only exist in derived games but share
        // the TES4 layout); records with unknown type ids are still parsed, they are
        // just reported verbatim.
        enum RecordTypes : std::uint32_t
        {
            REC_ACHR = ESM::fourCC("ACHR"), // placed NPC
            REC_ACRE = ESM::fourCC("ACRE"), // placed creature (Oblivion only)
            REC_ACTI = ESM::fourCC("ACTI"), // activator
            REC_ADDN = ESM::fourCC("ADDN"), // add-on node
            REC_ALCH = ESM::fourCC("ALCH"), // potion
            REC_AMMO = ESM::fourCC("AMMO"), // ammunition
            REC_ANIO = ESM::fourCC("ANIO"), // animated object
            REC_APPA = ESM::fourCC("APPA"), // alchemical apparatus
            REC_ARMA = ESM::fourCC("ARMA"), // armature (model)
            REC_ARMO = ESM::fourCC("ARMO"), // armor
            REC_ARTI = ESM::fourCC("ARTI"), // art object (unused?)
            REC_BOOK = ESM::fourCC("BOOK"), // book
            REC_BSGN = ESM::fourCC("BSGN"), // birth sign
            REC_CELL = ESM::fourCC("CELL"), // cell
            REC_CLAS = ESM::fourCC("CLAS"), // class
            REC_CLMT = ESM::fourCC("CLMT"), // climate
            REC_CLOT = ESM::fourCC("CLOT"), // clothing
            REC_CONT = ESM::fourCC("CONT"), // container
            REC_CREA = ESM::fourCC("CREA"), // creature
            REC_CSTY = ESM::fourCC("CSTY"), // combat style
            REC_DIAL = ESM::fourCC("DIAL"), // dialog topic
            REC_DOOR = ESM::fourCC("DOOR"), // door
            REC_ECZN = ESM::fourCC("ECZN"), // encounter zone
            REC_EFSH = ESM::fourCC("EFSH"), // effect shader
            REC_ENCH = ESM::fourCC("ENCH"), // enchantment
            REC_EYES = ESM::fourCC("EYES"), // eyes
            REC_FACT = ESM::fourCC("FACT"), // faction
            REC_FLOR = ESM::fourCC("FLOR"), // flora
            REC_FURN = ESM::fourCC("FURN"), // furniture
            REC_GLOB = ESM::fourCC("GLOB"), // global variable
            REC_GMST = ESM::fourCC("GMST"), // game setting
            REC_GRAS = ESM::fourCC("GRAS"), // grass
            REC_GRUP = ESM::fourCC("GRUP"), // group
            REC_HAIR = ESM::fourCC("HAIR"), // hair
            REC_IMAD = ESM::fourCC("IMAD"), // image space modifier
            REC_IMGS = ESM::fourCC("IMGS"), // image space
            REC_IDLE = ESM::fourCC("IDLE"), // idle animation
            REC_IDLM = ESM::fourCC("IDLM"), // idle marker
            REC_INGR = ESM::fourCC("INGR"), // ingredient
            REC_KEYM = ESM::fourCC("KEYM"), // key
            REC_LAND = ESM::fourCC("LAND"), // landscape
            REC_LGTM = ESM::fourCC("LGTM"), // lighting template
            REC_LIGH = ESM::fourCC("LIGH"), // light
            REC_LTEX = ESM::fourCC("LTEX"), // landscape texture
            REC_LVLC = ESM::fourCC("LVLC"), // leveled creature
            REC_LVLI = ESM::fourCC("LVLI"), // leveled item
            REC_LVLN = ESM::fourCC("LVLN"), // leveled actor
            REC_LVSP = ESM::fourCC("LVSP"), // leveled spell
            REC_MGEF = ESM::fourCC("MGEF"), // magic effect
            REC_MISC = ESM::fourCC("MISC"), // miscellaneous item
            REC_MOVT = ESM::fourCC("MOVT"), // movement type
            REC_MSTT = ESM::fourCC("MSTT"), // movable static
            REC_MUSC = ESM::fourCC("MUSC"), // music type
            REC_MUST = ESM::fourCC("MUST"), // music track
            REC_NOTE = ESM::fourCC("NOTE"), // note
            REC_NPC_ = ESM::fourCC("NPC_"), // actor (NPC)
            REC_OTFT = ESM::fourCC("OTFT"), // outfit
            REC_PACK = ESM::fourCC("PACK"), // AI package
            REC_PGRD = ESM::fourCC("PGRD"), // path grid (Oblivion only)
            REC_QUST = ESM::fourCC("QUST"), // quest
            REC_RACE = ESM::fourCC("RACE"), // race
            REC_REFR = ESM::fourCC("REFR"), // object reference
            REC_REGN = ESM::fourCC("REGN"), // region
            REC_ROAD = ESM::fourCC("ROAD"), // road (Oblivion only)
            REC_SBSP = ESM::fourCC("SBSP"), // subspace (Oblivion only)
            REC_SCPT = ESM::fourCC("SCPT"), // script
            REC_SCRL = ESM::fourCC("SCRL"), // scroll
            REC_SGST = ESM::fourCC("SGST"), // sigil stone
            REC_SKIL = ESM::fourCC("SKIL"), // skill
            REC_SLGM = ESM::fourCC("SLGM"), // soul gem
            REC_SOUN = ESM::fourCC("SOUN"), // sound
            REC_SPEL = ESM::fourCC("SPEL"), // spell
            REC_STAT = ESM::fourCC("STAT"), // static
            REC_TACT = ESM::fourCC("TACT"), // talking activator
            REC_TES4 = ESM::fourCC("TES4"), // plugin header
            REC_TREE = ESM::fourCC("TREE"), // tree
            REC_WATR = ESM::fourCC("WATR"), // water type
            REC_WEAP = ESM::fourCC("WEAP"), // weapon
            REC_WRLD = ESM::fourCC("WRLD"), // world space
            REC_WTHR = ESM::fourCC("WTHR"), // weather
        };

        bool isOblivionRecordType(std::uint32_t typeId);

#pragma pack(push, 1)
        // TES4 record header, 20 bytes.
        struct RecordTypeHeader
        {
            std::uint32_t typeId; // see RecordTypes
            std::uint32_t dataSize; // size of the record data, *not* including this header
            std::uint32_t flags; // see RecordFlag
            std::uint32_t formId; // unique identifier within the file/load order
            std::uint32_t versionControlInfo; // per-record timestamp/ownership, 0 if unused
        };

        // TES4 group header, 20 bytes.
        struct GroupHeader
        {
            std::uint32_t typeId; // always "GRUP"
            std::uint32_t groupSize; // size of the whole group, *including* this header
            std::uint32_t label; // format depends on the group type (fourCC, FormId or raw int)
            std::int32_t type; // see GroupType
            std::uint32_t stamp; // low byte: day of month, next byte: months since Dec 2002
                                 // (upper word: owner user id if version control is enabled)
        };

        // Sub-record header, 6 bytes.
        struct SubRecordHeader
        {
            std::uint32_t typeId;
            std::uint16_t dataSize; // may be overridden by a preceding XXXX subrecord
        };

        // HEDR sub-record, 12 bytes.
        struct Data
        {
            ESM::ESMVersion version; // file format version, see isOblivionHedrVersion
            std::int32_t records; // number of records and groups, not including the TES4 record
            std::uint32_t nextObjectId; // next available object id
        };
#pragma pack(pop)

        static_assert(sizeof(RecordTypeHeader) == 20, "TES4 record header must be 20 bytes");
        static_assert(sizeof(GroupHeader) == 20, "TES4 group header must be 20 bytes");
        static_assert(sizeof(SubRecordHeader) == 6, "TES4 sub record header must be 6 bytes");
        static_assert(sizeof(Data) == 12, "TES4 HEDR must be 12 bytes");

        // The Oblivion plugin/master format versions found in the HEDR record.
        // 0.8 is the version used by older plugins (the minimum supported by the game),
        // 1.0 is what vanilla Oblivion and the Construction Set write.
        bool isOblivionHedrVersion(std::uint32_t version);

        // A parsed TES4 (header) record.
        struct Header
        {
            std::uint32_t flags = 0; // see RecordFlag (Rec_ESM, Rec_Localized)
            Data mData{};
            std::string mAuthor; // CNAM
            std::string mDesc; // SNAM
            std::vector<ESM::MasterData> mMaster; // MAST/DATA pairs
            std::vector<std::uint32_t> mOverrides; // ONAM (only cell children's formIds)

            // True when the file is a master (.esm), false for plugins (.esp).
            // Determined by the Rec_ESM flag, not by the file extension.
            bool isMaster() const { return (flags & Rec_ESM) != 0; }
        };

        struct SubRecord
        {
            std::uint32_t type = 0; // fourCC, see e.g. ESM::fourCC("SPIT")
            std::vector<char> data; // raw sub-record data, with any XXXX size applied

            // Interprets the data as a zero-terminated string (everything up to the
            // first '\0', or the whole data if there is none).
            std::string asZString() const;

            // Reads exactly sizeof(T) bytes from the data. Throws std::runtime_error if
            // the data size does not match. T must be trivially copyable.
            template <typename T>
            T asExact() const
            {
                static_assert(std::is_trivially_copyable_v<T>, "T must be trivially copyable");
                if (data.size() != sizeof(T))
                    throw std::runtime_error("Oblivion subrecord " + ESM::printName(type) + " has size "
                        + std::to_string(data.size()) + ", expected " + std::to_string(sizeof(T)));

                T value{};
                std::memcpy(&value, data.data(), sizeof(T));
                return value;
            }
        };

        struct Record
        {
            // Marker for Record::groupIndex when the record is at the top level of the file.
            static constexpr std::uint32_t noGroup = 0xffffffffu;

            std::uint32_t typeId = 0; // fourCC, see RecordTypes
            std::uint32_t flags = 0; // see RecordFlag
            std::uint32_t formId = 0; // 0 for GMST records without a formId and for the TES4 record
            std::uint32_t versionControlInfo = 0;
            std::vector<SubRecord> subRecords;

            // Index into the groups vector of the innermost group enclosing this record,
            // or noGroup when the record is at the top level of the file. Together with
            // the groups' depths and labels this allows reconstructing the record's
            // position in the group hierarchy (e.g. which world space an exterior CELL
            // belongs to). Only meaningful after EsmFile::parse().
            std::uint32_t groupIndex = noGroup;

            bool hasFlag(std::uint32_t flag) const { return (flags & flag) != 0; }

            // Returns the first sub-record of the given type, or nullptr.
            const SubRecord* findSubRecord(std::uint32_t typeId) const;

            std::vector<const SubRecord*> findSubRecords(std::uint32_t typeId) const;
        };

        struct Group
        {
            std::uint32_t label = 0; // reinterpretation depends on the group type
            std::int32_t type = 0; // see GroupType
            std::uint32_t groupSize = 0; // includes the 20 byte group header
            // The raw version-control stamp of the group (see GroupHeader::stamp); low byte is the
            // day of the month, next byte the months since Dec 2002, high word the owner for versioned files.
            std::uint32_t versionControlInfo = 0;
            std::size_t depth = 0; // 0 = top level group
        };

        // Parses an Oblivion ESM/ESP file. After open() returns, the TES4 header record
        // has been read and validated and is available through header(); call parse() to
        // materialise the remaining records and groups into memory.
        //
        // Throws std::runtime_error on malformed files.
        class EsmFile
        {
        public:
            EsmFile();
            explicit EsmFile(const std::filesystem::path& path);
            EsmFile(const EsmFile&) = delete;
            EsmFile& operator=(const EsmFile&) = delete;
            ~EsmFile();

            // Opens the file and reads the TES4 header record. The rest of the file is
            // parsed by parse(). Throws std::runtime_error if the file is not an Oblivion
            // plugin (bad signature, truncated header, ...).
            void open(const std::filesystem::path& path);
            void open(Files::IStreamPtr&& stream, const std::filesystem::path& filename);

            // Resets the parser to its initial state (closes the stream, drops the data).
            void close();

            bool isOpen() const { return mOpen; }

            const std::filesystem::path& filename() const { return mFilename; }

            // The parsed TES4 header record. Valid after open().
            const Header& header() const { return mHeader; }

            // Parses the remainder of the file into memory. Can be called multiple times;
            // after the first call it is a no-op.
            void parse();

            // The parsed records (in file order, not including the TES4 header record)
            // and groups. Only valid after parse() has returned.
            const std::vector<Record>& records() const { return mRecords; }
            const std::vector<Group>& groups() const { return mGroups; }

        private:
            void readHeaderRecord();

            bool mOpen = false;
            bool mParsed = false;
            std::filesystem::path mFilename;
            Files::IStreamPtr mStream;
            Header mHeader;
            std::vector<Record> mRecords;
            std::vector<Group> mGroups;
        };

        // SPIT, 16 bytes. The basic spell data.
        struct SpellData
        {
            std::uint32_t type = 0; // see SpellType
            std::uint32_t cost = 0; // spell cost if the autocalc flag is off
            std::uint32_t level = 0; // see SkillLevel
            std::uint32_t flags = 0; // see SpellFlag
        };

        enum SpellType : std::uint32_t
        {
            Spell_Spell = 0,
            Spell_Disease = 1,
            Spell_Power = 2,
            Spell_LesserPower = 3,
            Spell_Ability = 4,
            Spell_Poison = 5,
        };

        enum SkillLevel : std::uint32_t
        {
            Level_Apprentice = 0,
            Level_Novice = 1,
            Level_Journeyman = 2,
            Level_Expert = 3,
            Level_Master = 4,
        };

        enum SpellFlag : std::uint32_t
        {
            Spell_ManualCost = 0x00000001, // do not autocalc the spell cost
            Spell_PlayerStartSpell = 0x00000004,
            Spell_AreaEffectIgnoresLOS = 0x00000010,
            Spell_ScriptEffectAlwaysApplies = 0x00000020,
            Spell_DisallowAbsorbReflect = 0x00000040,
        };

        // ENIT, 16 bytes. The basic enchantment data.
        struct EnchantmentData
        {
            std::uint32_t type = 0; // see EnchantmentType
            std::uint32_t chargeAmount = 0; // manual charge value, used if IsManualCost is set
            std::uint32_t enchantCost = 0; // manual enchantment cost, used if IsManualCost is set
            std::uint32_t flags = 0; // see EnchantmentFlag
        };

        enum EnchantmentType : std::uint32_t
        {
            Enchantment_Scroll = 0,
            Enchantment_Staff = 1,
            Enchantment_Weapon = 2,
            Enchantment_Apparel = 3,
        };

        enum EnchantmentFlag : std::uint32_t
        {
            Enchantment_ManualCost = 0x00000001, // do not autocalc the enchantment cost
        };

        // EFIT, 24 bytes. One magic effect as found in SPEL/ENCH records.
        // (Oblivion also shows this in SGST and ALCH records.)
        struct EffectItem
        {
            std::uint32_t effectId = 0; // fourCC of the magic effect, e.g. 'FIDG'
            std::uint32_t magnitude = 0;
            std::uint32_t area = 0;
            std::uint32_t duration = 0;
            std::uint32_t deliveryType = 0; // see DeliveryType
            std::uint32_t actorValue = 0; // actor value subtype, e.g. for fortify attribute
        };

        enum DeliveryType : std::uint32_t
        {
            Delivery_Self = 0,
            Delivery_Touch = 1,
            Delivery_Target = 2,
        };

        // SCIT, variable (usually 16 bytes). Script effect data; fields that are missing
        // in shorter SCIT sub-records default to 0.
        struct ScriptEffectItem
        {
            std::uint32_t scriptFormId = 0; // 0 = no script
            std::uint32_t school = 0; // see SchoolType
            std::uint32_t visualEffectId = 0; // fourCC used as visual effect, 0 = NONE
            std::uint32_t flags = 0; // see ScriptEffectFlag
        };

        enum SchoolType : std::uint32_t
        {
            School_Alteration = 0,
            School_Conjuration = 1,
            School_Destruction = 2,
            School_Illusion = 3,
            School_Mysticism = 4,
            School_Restoration = 5,
        };

        enum ScriptEffectFlag : std::uint32_t
        {
            ScriptEffect_Hostile = 0x00000001,
        };

        // One EFID/EFIT pair, optionally followed by SCIT/FULL (script effects).
        struct MagicEffect
        {
            EffectItem effect; // from EFIT (effect.effectId duplicates the EFID content)
            std::optional<ScriptEffectItem> script; // from SCIT, if present
            std::string scriptName; // FULL sub-record following a SCIT, if present
        };

        struct Spell
        {
            std::string editorId; // EDID
            std::string name; // first FULL
            SpellData data; // SPIT
            std::vector<MagicEffect> effects; // EFID/EFIT(/SCIT/FULL) groups
        };

        struct Enchantment
        {
            std::string editorId; // EDID
            std::string name; // FULL, if present
            EnchantmentData data; // ENIT
            std::vector<MagicEffect> effects; // EFID/EFIT(/SCIT/FULL) groups
        };

        // Decodes the Oblivion-specific SPEL and ENCH record payloads.
        // Throws std::runtime_error on malformed records.
        Spell decodeSpell(const Record& record);
        Enchantment decodeEnchantment(const Record& record);
    }
}

#endif
