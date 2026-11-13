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
    /// 'f' -> float, 's' -> short, 'l' -> long. An unrecognised FNAM type or an empty editor id yields \a
    /// std::nullopt.
    ///
    /// \note Integer-typed globals ('s'/'l') rely on the TES4 float-bit FLTV convention: ESM4::GlobalVariable::load
    /// always reads the 4-byte FLTV subrecord as a float, and Oblivion stores integer global values as the float
    /// value itself (e.g. 1100 is stored as the bit pattern of 1100.0f), so narrowing that float to int32 recovers
    /// the intended number. A TES4-family file that instead stored true integer-encoded FLTV bytes (42 as
    /// 0x0000002A) would read as a denormal ~4e-43 and silently bridge to 0; no such file is known, and supporting
    /// one would require the reader to expose the raw uint32 for non-'f' FNAM types.
    std::optional<ESM::Global> convertToESM3(const GlobalVariable& glob);
}

#endif // OPENMW_COMPONENTS_ESM4_CONVERTGLOB_HPP
