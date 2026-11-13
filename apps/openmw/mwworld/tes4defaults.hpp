#ifndef OPENMW_MWWORLD_TES4DEFAULTS_H
#define OPENMW_MWWORLD_TES4DEFAULTS_H

namespace MWWorld
{
    class ESMStore;

    /// Insert mandatory ESM3 game settings that TES4 (Oblivion) content files do not provide but engine code
    /// dereferences unconditionally. Only records missing from the store are inserted; existing records are left
    /// untouched. Call for TES4-only sessions only.
    void seedTES4DefaultGameSettings(ESMStore& store);

    /// Insert the Morrowind-dialect skill vocabulary (SKIL records) that engine code and Lua mechanics scripts
    /// dereference unconditionally but TES4 content does not bridge. Only records missing from the store are
    /// inserted. Call for TES4-only sessions only.
    void seedTES4DefaultSkills(ESMStore& store);

    /// Mirror the openmw.cfg fallback entries (FontColor_*, Movies_* etc.) into the GMST store so Lua UI scripts
    /// that read them via core.getGMST() resolve. Only records missing from the store are inserted. Call for
    /// TES4-only sessions only.
    void seedTES4FallbackGameSettings(ESMStore& store);

    /// Insert a minimal synthetic "Player" NPC with a synthetic race (esm3 dialect) when content provides none.
    /// The Morrowind-dialect runtime requires both to initialise the world; TES4 content provides no equivalent
    /// usable records (vanilla Oblivion's player base object is flagged invalid). Only inserted when missing.
    /// Call for TES4-only sessions only.
    void seedTES4DefaultPlayer(ESMStore& store);
}

#endif
