#ifndef OPENMW_MWWORLD_TES4DEFAULTS_H
#define OPENMW_MWWORLD_TES4DEFAULTS_H

namespace MWWorld
{
    class ESMStore;

    /// Insert mandatory ESM3 game settings that TES4 (Oblivion) content files do not provide but engine code
    /// dereferences unconditionally. Only records missing from the store are inserted; existing records are left
    /// untouched. Call for TES4-only sessions only.
    void seedTES4DefaultGameSettings(ESMStore& store);
}

#endif
