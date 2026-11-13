#ifndef OPENMW_COMPONENTS_ESM4_CONVERTGLOB_HPP
#define OPENMW_COMPONENTS_ESM4_CONVERTGLOB_HPP

#include <optional>

namespace ESM
{
    struct Global;
}

namespace ESM4
{
    struct GlobalVariable;

    /// Convert a TES4 (ESM4) GLOB record into the esm3 \a ESM::Global representation so that it can be stored in
    /// the engine's \c Store<ESM::Global>. The FNAM type letter is mapped to the matching esm3 value type:
    /// 'f' -> float, 's' -> short, 'l' -> long. An unrecognised FNAM type yields \a std::nullopt.
    std::optional<ESM::Global> convertToESM3(const GlobalVariable& glob);
}

#endif // OPENMW_COMPONENTS_ESM4_CONVERTGLOB_HPP
