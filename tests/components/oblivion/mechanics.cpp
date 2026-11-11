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

#include "mechanics.hpp"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <limits>
#include <unordered_map>
#include <utility>

#include <components/esm4/magiceffectid.hpp>

namespace Oblivion
{
    namespace
    {
        constexpr std::int32_t clampInt(std::int32_t value, std::int32_t min, std::int32_t max) noexcept
        {
            return std::clamp(value, min, max);
        }

        // Round half up, minimum 1 (per the Oblivion rounding rule for potion magnitudes).
        std::int32_t roundMinOne(float value) noexcept
        {
            if (value <= 1.f)
                return 1;
            return std::max(1, static_cast<std::int32_t>(std::floor(value + 0.5f)));
        }

        // Effect ids as defined by ESM4::MagicEffectID; aliased here for readability.
        constexpr std::uint32_t FX_BRDN = ESM4::EFI_BRDN;
        constexpr std::uint32_t FX_FTHR = ESM4::EFI_FTHR;
        constexpr std::uint32_t FX_FISH = ESM4::EFI_FISH;
        constexpr std::uint32_t FX_FRSH = ESM4::EFI_FRSH;
        constexpr std::uint32_t FX_OPEN = ESM4::EFI_OPEN;
        constexpr std::uint32_t FX_SHLD = ESM4::EFI_SHLD;
        constexpr std::uint32_t FX_LISH = ESM4::EFI_LISH;
        constexpr std::uint32_t FX_WABR = ESM4::EFI_WABR;
        constexpr std::uint32_t FX_WAWA = ESM4::EFI_WAWA;
        constexpr std::uint32_t FX_BABO = ESM4::EFI_BABO;
        constexpr std::uint32_t FX_BACU = ESM4::EFI_BACU;
        constexpr std::uint32_t FX_BAGA = ESM4::EFI_BAGA;
        constexpr std::uint32_t FX_BAGR = ESM4::EFI_BAGR;
        constexpr std::uint32_t FX_BAHE = ESM4::EFI_BAHE;
        constexpr std::uint32_t FX_BASH = ESM4::EFI_BASH;
        constexpr std::uint32_t FX_BWAX = ESM4::EFI_BWAX;
        constexpr std::uint32_t FX_BWBO = ESM4::EFI_BWBO;
        constexpr std::uint32_t FX_BWDA = ESM4::EFI_BWDA;
        constexpr std::uint32_t FX_BWMA = ESM4::EFI_BWMA;
        constexpr std::uint32_t FX_BWSW = ESM4::EFI_BWSW;
        constexpr std::uint32_t FX_Z001 = ESM4::EFI_Z001;
        constexpr std::uint32_t FX_Z002 = ESM4::EFI_Z002;
        constexpr std::uint32_t FX_Z003 = ESM4::EFI_Z003;
        constexpr std::uint32_t FX_Z005 = ESM4::EFI_Z005;
        constexpr std::uint32_t FX_ZCLA = ESM4::EFI_ZCLA;
        constexpr std::uint32_t FX_ZDAE = ESM4::EFI_ZDAE;
        constexpr std::uint32_t FX_ZDRE = ESM4::EFI_ZDRE;
        constexpr std::uint32_t FX_ZDRL = ESM4::EFI_ZDRL;
        constexpr std::uint32_t FX_ZFIA = ESM4::EFI_ZFIA;
        constexpr std::uint32_t FX_ZFRA = ESM4::EFI_ZFRA;
        constexpr std::uint32_t FX_ZGHO = ESM4::EFI_ZGHO;
        constexpr std::uint32_t FX_ZHDZ = ESM4::EFI_ZHDZ;
        constexpr std::uint32_t FX_ZLIC = ESM4::EFI_ZLIC;
        constexpr std::uint32_t FX_ZSCA = ESM4::EFI_ZSCA;
        constexpr std::uint32_t FX_ZSKE = ESM4::EFI_ZSKE;
        constexpr std::uint32_t FX_ZSKA = ESM4::EFI_ZSKA;
        constexpr std::uint32_t FX_ZSKH = ESM4::EFI_ZSKH;
        constexpr std::uint32_t FX_ZSKC = ESM4::EFI_ZSKC;
        constexpr std::uint32_t FX_ZSPD = ESM4::EFI_ZSPD;
        constexpr std::uint32_t FX_ZSTA = ESM4::EFI_ZSTA;
        constexpr std::uint32_t FX_ZWRA = ESM4::EFI_ZWRA;
        constexpr std::uint32_t FX_ZWRL = ESM4::EFI_ZWRL;
        constexpr std::uint32_t FX_ZXIV = ESM4::EFI_ZXIV;
        constexpr std::uint32_t FX_ZZOM = ESM4::EFI_ZZOM;
        constexpr std::uint32_t FX_TURN = ESM4::EFI_TURN;
        constexpr std::uint32_t FX_DGAT = ESM4::EFI_DGAT;
        constexpr std::uint32_t FX_DGFA = ESM4::EFI_DGFA;
        constexpr std::uint32_t FX_DGHE = ESM4::EFI_DGHE;
        constexpr std::uint32_t FX_DGSP = ESM4::EFI_DGSP;
        constexpr std::uint32_t FX_DIAR = ESM4::EFI_DIAR;
        constexpr std::uint32_t FX_DIWE = ESM4::EFI_DIWE;
        constexpr std::uint32_t FX_DRAT = ESM4::EFI_DRAT;
        constexpr std::uint32_t FX_DRFA = ESM4::EFI_DRFA;
        constexpr std::uint32_t FX_DRHE = ESM4::EFI_DRHE;
        constexpr std::uint32_t FX_DRSP = ESM4::EFI_DRSP;
        constexpr std::uint32_t FX_DRSK = ESM4::EFI_DRSK;
        constexpr std::uint32_t FX_FIDG = ESM4::EFI_FIDG;
        constexpr std::uint32_t FX_FRDG = ESM4::EFI_FRDG;
        constexpr std::uint32_t FX_SHDG = ESM4::EFI_SHDG;
        constexpr std::uint32_t FX_WKDI = ESM4::EFI_WKDI;
        constexpr std::uint32_t FX_WKFI = ESM4::EFI_WKFI;
        constexpr std::uint32_t FX_WKFR = ESM4::EFI_WKFR;
        constexpr std::uint32_t FX_WKMA = ESM4::EFI_WKMA;
        constexpr std::uint32_t FX_WKNW = ESM4::EFI_WKNW;
        constexpr std::uint32_t FX_WKPO = ESM4::EFI_WKPO;
        constexpr std::uint32_t FX_WKSH = ESM4::EFI_WKSH;
        constexpr std::uint32_t FX_CALM = ESM4::EFI_CALM;
        constexpr std::uint32_t FX_CHML = ESM4::EFI_CHML;
        constexpr std::uint32_t FX_CHRM = ESM4::EFI_CHRM;
        constexpr std::uint32_t FX_COCR = ESM4::EFI_COCR;
        constexpr std::uint32_t FX_COHU = ESM4::EFI_COHU;
        constexpr std::uint32_t FX_DEMO = ESM4::EFI_DEMO;
        constexpr std::uint32_t FX_FRNZ = ESM4::EFI_FRNZ;
        constexpr std::uint32_t FX_INVI = ESM4::EFI_INVI;
        constexpr std::uint32_t FX_LGHT = ESM4::EFI_LGHT;
        constexpr std::uint32_t FX_NEYE = ESM4::EFI_NEYE;
        constexpr std::uint32_t FX_PARA = ESM4::EFI_PARA;
        constexpr std::uint32_t FX_RALY = ESM4::EFI_RALY;
        constexpr std::uint32_t FX_SLNC = ESM4::EFI_SLNC;
        constexpr std::uint32_t FX_DTCT = ESM4::EFI_DTCT;
        constexpr std::uint32_t FX_DSPL = ESM4::EFI_DSPL;
        constexpr std::uint32_t FX_REDG = ESM4::EFI_REDG;
        constexpr std::uint32_t FX_RFLC = ESM4::EFI_RFLC;
        constexpr std::uint32_t FX_STRP = ESM4::EFI_STRP;
        constexpr std::uint32_t FX_SABS = ESM4::EFI_SABS;
        constexpr std::uint32_t FX_TELE = ESM4::EFI_TELE;
        constexpr std::uint32_t FX_ABAT = ESM4::EFI_ABAT;
        constexpr std::uint32_t FX_ABFA = ESM4::EFI_ABFA;
        constexpr std::uint32_t FX_ABHE = ESM4::EFI_ABHe;
        constexpr std::uint32_t FX_ABSP = ESM4::EFI_ABSP;
        constexpr std::uint32_t FX_ABSK = ESM4::EFI_ABSK;
        // Cure Disease (the fourCC "CUDI" is not used by the esm4 reader)
        constexpr std::uint32_t FX_CUDI = ESM4::EFI_1400;
        constexpr std::uint32_t FX_CUPA = ESM4::EFI_CUPA;
        constexpr std::uint32_t FX_CUPO = ESM4::EFI_CUPO;
        constexpr std::uint32_t FX_FOAT = ESM4::EFI_FOAT;
        constexpr std::uint32_t FX_FOFA = ESM4::EFI_FOFA;
        constexpr std::uint32_t FX_FOHE = ESM4::EFI_FOHE;
        constexpr std::uint32_t FX_FOSP = ESM4::EFI_FOSP;
        constexpr std::uint32_t FX_FOSK = ESM4::EFI_FOSK;
        constexpr std::uint32_t FX_RSDI = ESM4::EFI_RSDI;
        constexpr std::uint32_t FX_RSFI = ESM4::EFI_RSFI;
        constexpr std::uint32_t FX_RSFR = ESM4::EFI_RSFR;
        constexpr std::uint32_t FX_RSMA = ESM4::EFI_RSMA;
        constexpr std::uint32_t FX_RSNW = ESM4::EFI_RSNW;
        constexpr std::uint32_t FX_RSPA = ESM4::EFI_RSPA;
        constexpr std::uint32_t FX_RSPO = ESM4::EFI_RSPO;
        constexpr std::uint32_t FX_RSSH = ESM4::EFI_RSSH;
        constexpr std::uint32_t FX_REAT = ESM4::EFI_REAT;
        constexpr std::uint32_t FX_REFA = ESM4::EFI_REFA;
        constexpr std::uint32_t FX_REHE = ESM4::EFI_REHE;
        constexpr std::uint32_t FX_RESP = ESM4::EFI_RESP;
        constexpr std::uint32_t FX_LOCK = ESM4::EFI_LOCK;
        constexpr std::uint32_t FX_SEFF = ESM4::EFI_SEFF;
        constexpr std::uint32_t FX_Z020 = ESM4::EFI_Z020;
        constexpr std::uint32_t FX_MYHL = ESM4::EFI_MYHL;
        constexpr std::uint32_t FX_MYTH = ESM4::EFI_MYTH;
        constexpr std::uint32_t FX_REAN = ESM4::EFI_REAN;
        constexpr std::uint32_t FX_DISE = ESM4::EFI_DISE;
        constexpr std::uint32_t FX_POSN = ESM4::EFI_POSN;
        constexpr std::uint32_t FX_DUMY = ESM4::EFI_DUMY;
        constexpr std::uint32_t FX_STMA = ESM4::EFI_STMA;
        constexpr std::uint32_t FX_SUDG = ESM4::EFI_SUDG;
        constexpr std::uint32_t FX_VAMP = ESM4::EFI_VAMP;
        constexpr std::uint32_t FX_DARK = ESM4::EFI_DARK;
        constexpr std::uint32_t FX_RSWD = ESM4::EFI_RSWD;

        constexpr MagicSchool ALT = MagicSchool::Alteration;
        constexpr MagicSchool CON = MagicSchool::Conjuration;
        constexpr MagicSchool DES = MagicSchool::Destruction;
        constexpr MagicSchool ILL = MagicSchool::Illusion;
        constexpr MagicSchool MYS = MagicSchool::Mysticism;
        constexpr MagicSchool RES = MagicSchool::Restoration;

        // The vanilla Oblivion spell effect table. Base costs and barter factors from
        // https://en.uesp.net/wiki/Oblivion:Spell_Effects (current as of the page
        // revision of 2025-11-01). mDurationOnly / mMagnitudeOnly reflect the two
        // parameter shapes, mConstantEnchantable marks effects usable as worn
        // constant-effect enchantments, mHarmful marks effects that are considered
        // negative for alchemy purposes (poisons vs potions and Alembic rules).
        constexpr std::array<SpellEffectInfo, 126> sSpellEffects = {{
            // Alteration
            { FX_BRDN, ALT, 0.21f, 0.f, false, false, false, true },
            { FX_FTHR, ALT, 0.01f, 25.f, false, false, true, false },
            { FX_FISH, ALT, 0.95f, 100.f, false, false, true, false },
            { FX_FRSH, ALT, 0.95f, 100.f, false, false, true, false },
            { FX_OPEN, ALT, 4.30f, 0.f, false, false, false, false },
            { FX_SHLD, ALT, 0.45f, 100.f, false, false, true, false },
            { FX_LISH, ALT, 0.95f, 100.f, false, false, true, false },
            { FX_WABR, ALT, 14.50f, 400.f, true, false, true, false },
            { FX_WAWA, ALT, 13.00f, 400.f, true, false, true, false },
            // Conjuration
            { FX_BABO, CON, 12.00f, 0.f, false, false, false, false },
            { FX_BACU, CON, 12.00f, 0.f, false, false, false, false },
            { FX_BAGA, CON, 8.00f, 0.f, false, false, false, false },
            { FX_BAGR, CON, 12.00f, 0.f, false, false, false, false },
            { FX_BAHE, CON, 12.00f, 0.f, false, false, false, false },
            { FX_BASH, CON, 18.00f, 0.f, false, false, false, false },
            { FX_BWAX, CON, 39.00f, 0.f, false, false, false, false },
            { FX_BWBO, CON, 95.00f, 0.f, false, false, false, false },
            { FX_BWDA, CON, 14.00f, 0.f, false, false, false, false },
            { FX_BWMA, CON, 91.00f, 0.f, false, false, false, false },
            { FX_BWSW, CON, 235.00f, 0.f, false, false, false, false },
            { FX_Z001, CON, 13.00f, 0.f, false, false, false, false },
            { FX_Z002, CON, 33.30f, 0.f, false, false, false, false },
            { FX_Z003, CON, 45.00f, 0.f, false, false, false, false },
            { FX_Z005, CON, 47.30f, 0.f, false, false, false, false },
            { FX_ZCLA, CON, 75.56f, 0.f, false, false, false, false },
            { FX_ZDAE, CON, 123.33f, 0.f, false, false, false, false },
            { FX_ZDRE, CON, 72.50f, 0.f, false, false, false, false },
            { FX_ZDRL, CON, 157.14f, 0.f, false, false, false, false },
            { FX_ZFIA, CON, 45.00f, 0.f, false, false, false, false },
            { FX_ZFRA, CON, 102.86f, 0.f, false, false, false, false },
            { FX_ZGHO, CON, 22.00f, 0.f, false, false, false, false },
            { FX_ZHDZ, CON, 56.00f, 0.f, false, false, false, false },
            { FX_ZLIC, CON, 350.00f, 0.f, false, false, false, false },
            { FX_ZSCA, CON, 30.00f, 0.f, false, false, false, false },
            { FX_ZSKE, CON, 11.25f, 0.f, false, false, false, false },
            { FX_ZSKA, CON, 32.50f, 0.f, false, false, false, false },
            { FX_ZSKH, CON, 66.00f, 0.f, false, false, false, false },
            { FX_ZSKC, CON, 152.00f, 0.f, false, false, false, false },
            { FX_ZSPD, CON, 195.00f, 0.f, false, false, false, false },
            { FX_ZSTA, CON, 125.00f, 0.f, false, false, false, false },
            { FX_ZWRA, CON, 87.50f, 0.f, false, false, false, false },
            { FX_ZWRL, CON, 260.00f, 0.f, false, false, false, false },
            { FX_ZXIV, CON, 200.00f, 0.f, false, false, false, false },
            { FX_ZZOM, CON, 16.67f, 0.f, false, false, false, false },
            { FX_TURN, CON, 0.083f, 0.f, false, false, false, true },
            // Destruction
            { FX_DGAT, DES, 100.00f, 0.f, false, false, false, true },
            { FX_DGFA, DES, 4.40f, 0.f, false, false, false, true },
            { FX_DGHE, DES, 12.00f, 0.f, false, false, false, true },
            { FX_DGSP, DES, 2.45f, 0.f, false, false, false, true },
            { FX_DIAR, DES, 6.20f, 0.f, false, false, false, true },
            { FX_DIWE, DES, 6.20f, 0.f, false, false, false, true },
            { FX_DRAT, DES, 0.70f, 0.f, false, false, false, true },
            { FX_DRFA, DES, 0.18f, 0.f, false, false, false, true },
            { FX_DRHE, DES, 0.90f, 0.f, false, false, false, true },
            { FX_DRSP, DES, 0.18f, 0.f, false, false, false, true },
            { FX_DRSK, DES, 0.65f, 0.f, false, false, false, true },
            { FX_FIDG, DES, 7.50f, 0.f, false, false, false, true },
            { FX_FRDG, DES, 7.40f, 0.f, false, false, false, true },
            { FX_SHDG, DES, 7.80f, 0.f, false, false, false, true },
            { FX_WKDI, DES, 0.12f, 0.f, false, false, false, true },
            { FX_WKFI, DES, 0.10f, 0.f, false, false, false, true },
            { FX_WKFR, DES, 0.10f, 0.f, false, false, false, true },
            { FX_WKMA, DES, 0.25f, 0.f, false, false, false, true },
            { FX_WKNW, DES, 0.25f, 0.f, false, false, false, true },
            { FX_WKPO, DES, 0.10f, 0.f, false, false, false, true },
            { FX_WKSH, DES, 0.10f, 0.f, false, false, false, true },
            // Illusion
            { FX_CALM, ILL, 0.47f, 0.f, false, false, false, true },
            { FX_CHML, ILL, 0.63f, 65.f, false, false, true, false },
            { FX_CHRM, ILL, 0.20f, 0.f, false, false, false, true },
            { FX_COCR, ILL, 0.60f, 0.f, false, false, false, true },
            { FX_COHU, ILL, 0.75f, 0.f, false, false, false, true },
            { FX_DEMO, ILL, 0.49f, 0.f, false, false, false, true },
            { FX_FRNZ, ILL, 0.04f, 0.f, false, false, false, true },
            { FX_INVI, ILL, 40.00f, 0.f, true, false, false, false },
            { FX_LGHT, ILL, 0.051f, 12.5f, false, false, true, false },
            { FX_NEYE, ILL, 22.00f, 20.f, true, false, true, false },
            { FX_PARA, ILL, 475.00f, 0.f, true, false, false, true },
            { FX_RALY, ILL, 0.03f, 0.f, false, false, false, false },
            { FX_SLNC, ILL, 60.00f, 0.f, true, false, false, true },
            // Mysticism
            { FX_DTCT, MYS, 0.08f, 15.f, false, false, true, false },
            { FX_DSPL, MYS, 3.60f, 0.f, false, true, false, false },
            { FX_REDG, MYS, 2.50f, 400.f, false, false, false, false },
            { FX_RFLC, MYS, 3.50f, 400.f, false, false, false, false },
            { FX_STRP, MYS, 30.00f, 0.f, false, false, false, false },
            { FX_SABS, MYS, 3.00f, 400.f, false, false, false, false },
            { FX_TELE, MYS, 0.49f, 0.f, false, false, false, false },
            // Restoration
            { FX_ABAT, RES, 0.95f, 0.f, false, false, false, false },
            { FX_ABFA, RES, 6.00f, 0.f, false, false, false, false },
            { FX_ABHE, RES, 16.00f, 0.f, false, false, false, false },
            { FX_ABSP, RES, 7.50f, 0.f, false, false, false, false },
            { FX_ABSK, RES, 2.10f, 0.f, false, false, false, false },
            { FX_CUDI, RES, 1400.00f, 0.f, false, false, false, false },
            { FX_CUPA, RES, 500.00f, 0.f, false, false, false, false },
            { FX_CUPO, RES, 600.00f, 0.f, false, false, false, false },
            { FX_FOAT, RES, 0.60f, 100.f, false, false, true, false },
            { FX_FOFA, RES, 0.04f, 25.f, false, false, true, false },
            { FX_FOHE, RES, 0.14f, 150.f, false, false, true, false },
            { FX_FOSP, RES, 0.15f, 100.f, false, false, true, false },
            { FX_FOSK, RES, 0.60f, 100.f, false, false, true, false },
            { FX_RSDI, RES, 0.50f, 15.f, false, false, true, false },
            { FX_RSFI, RES, 0.50f, 50.f, false, false, true, false },
            { FX_RSFR, RES, 0.50f, 50.f, false, false, true, false },
            { FX_RSMA, RES, 2.00f, 150.f, false, false, true, false },
            { FX_RSNW, RES, 1.50f, 300.f, false, false, false, false },
            { FX_RSPA, RES, 0.75f, 30.f, false, false, true, false },
            { FX_RSPO, RES, 0.50f, 15.f, false, false, true, false },
            { FX_RSSH, RES, 0.50f, 50.f, false, false, true, false },
            { FX_REAT, RES, 38.00f, 0.f, false, false, false, false },
            { FX_REFA, RES, 2.00f, 0.f, false, false, false, false },
            { FX_REHE, RES, 10.00f, 0.f, false, false, false, false },
            { FX_RESP, RES, 2.50f, 0.f, false, false, false, false },
            // Special effects (school assignment is nominal; not usable for spellmaking)
            { FX_LOCK, ALT, 30.00f, 0.f, false, false, false, false },
            { FX_SEFF, ALT, 0.00f, 0.f, false, false, false, false },
            { FX_Z020, CON, 0.00f, 0.f, false, false, false, false },
            { FX_MYHL, CON, 110.00f, 0.f, false, false, false, false },
            { FX_MYTH, CON, 120.00f, 0.f, false, false, false, false },
            { FX_REAN, CON, 10.00f, 0.f, false, false, false, false },
            { FX_DISE, RES, 0.00f, 1.f, false, false, false, false },
            { FX_POSN, RES, 0.00f, 0.f, false, false, false, false },
            { FX_DUMY, DES, 0.00f, 1.f, false, false, false, true },
            { FX_STMA, DES, 0.00f, 0.f, false, false, false, false },
            { FX_SUDG, DES, 9.00f, 0.f, false, false, false, true },
            { FX_VAMP, RES, 0.00f, 0.f, false, false, false, false },
            { FX_DARK, DES, 0.00f, 0.f, false, false, false, false },
            { FX_RSWD, DES, 0.00f, 1.f, false, false, false, false },
        }};

        // Constant-effect (worn apparel) enchantment magnitudes per soul gem level (1-5).
        // Effects not listed here cannot be used as constant-effect enchantments.
        // (https://en.uesp.net/wiki/Oblivion:Enchanting#Worn_Enchantments)
        // -1 for flat-cost effects (always magnitude 5, independent of soul level).
        constexpr std::int32_t FLAT = -1;
        struct ConstantEnchantRow
        {
            std::uint32_t mEffectId;
            std::int32_t mMagnitudes[5];
        };
        constexpr std::array<ConstantEnchantRow, 23> sConstantMagnitudes = {{
            { FX_CHML, { 8, 11, 14, 17, 20 } },
            { FX_DTCT, { 8, 11, 14, 17, 20 } },
            { FX_FTHR, { 14, 23, 32, 41, 50 } },
            { FX_FISH, { 6, 6, 7, 8, 9 } },
            { FX_FOAT, { 6, 7, 8, 9, 10 } },
            { FX_FOFA, { 9, 13, 17, 21, 25 } },
            { FX_FOHE, { 6, 7, 8, 9, 10 } },
            { FX_FOSP, { 9, 13, 16, 20, 24 } },
            { FX_FOSK, { 6, 7, 8, 9, 10 } },
            { FX_FRSH, { 6, 6, 7, 8, 9 } },
            { FX_LISH, { 6, 6, 7, 8, 9 } },
            { FX_LGHT, { 12, 19, 26, 33, 40 } },
            { FX_NEYE, { FLAT, FLAT, FLAT, FLAT, FLAT } },
            { FX_RSDI, { 10, 15, 20, 25, 30 } },
            { FX_RSFI, { 10, 15, 20, 25, 30 } },
            { FX_RSFR, { 10, 15, 20, 25, 30 } },
            { FX_RSSH, { 10, 15, 20, 25, 30 } },
            { FX_RSMA, { 10, 15, 20, 25, 30 } },
            { FX_RSPA, { 10, 15, 20, 25, 30 } },
            { FX_RSPO, { 14, 23, 32, 41, 50 } },
            { FX_SHLD, { 6, 7, 8, 9, 10 } },
            { FX_WABR, { FLAT, FLAT, FLAT, FLAT, FLAT } },
            { FX_WAWA, { FLAT, FLAT, FLAT, FLAT, FLAT } },
        }};

        constexpr float kAlchemyMortarScale = 25.f; // mortar strength * 25 is added to effective Alchemy

        float calcinatorStrength(const AlchemyApparatus& apparatus) noexcept
        {
            return apparatus.mHasCalcinator ? apparatusStrength(apparatus.mCalcinator) : 0.f;
        }

        float retortStrength(const AlchemyApparatus& apparatus) noexcept
        {
            return apparatus.mHasRetort ? apparatusStrength(apparatus.mRetort) : 0.f;
        }

        float alembicStrength(const AlchemyApparatus& apparatus) noexcept
        {
            return apparatus.mHasAlembic ? apparatusStrength(apparatus.mAlembic) : 0.f;
        }
    } // namespace

    /* ============ skills ============ */

    std::string_view skillName(Skill skill)
    {
        switch (skill)
        {
            case Skill::Armorer: return "Armorer";
            case Skill::Athletics: return "Athletics";
            case Skill::Blade: return "Blade";
            case Skill::Block: return "Block";
            case Skill::Blunt: return "Blunt";
            case Skill::HandToHand: return "Hand to Hand";
            case Skill::HeavyArmor: return "Heavy Armor";
            case Skill::Alchemy: return "Alchemy";
            case Skill::Alteration: return "Alteration";
            case Skill::Conjuration: return "Conjuration";
            case Skill::Destruction: return "Destruction";
            case Skill::Illusion: return "Illusion";
            case Skill::Mysticism: return "Mysticism";
            case Skill::Restoration: return "Restoration";
            case Skill::Acrobatics: return "Acrobatics";
            case Skill::LightArmor: return "Light Armor";
            case Skill::Marksman: return "Marksman";
            case Skill::Mercantile: return "Mercantile";
            case Skill::Security: return "Security";
            case Skill::Sneak: return "Sneak";
            case Skill::Speechcraft: return "Speechcraft";
        }
        return "Unknown";
    }

    std::string_view attributeName(Attribute attribute)
    {
        switch (attribute)
        {
            case Attribute::Strength: return "Strength";
            case Attribute::Intelligence: return "Intelligence";
            case Attribute::Willpower: return "Willpower";
            case Attribute::Agility: return "Agility";
            case Attribute::Speed: return "Speed";
            case Attribute::Endurance: return "Endurance";
            case Attribute::Personality: return "Personality";
            case Attribute::Luck: return "Luck";
        }
        return "Unknown";
    }

    std::string_view specializationName(Specialization specialization)
    {
        switch (specialization)
        {
            case Specialization::Combat: return "Combat";
            case Specialization::Magic: return "Magic";
            case Specialization::Stealth: return "Stealth";
        }
        return "Unknown";
    }

    Attribute governingAttribute(Skill skill)
    {
        switch (skill)
        {
            case Skill::Blade:
            case Skill::Blunt:
            case Skill::HandToHand:
                return Attribute::Strength;
            case Skill::Armorer:
            case Skill::Block:
            case Skill::HeavyArmor:
                return Attribute::Endurance;
            case Skill::Athletics:
            case Skill::Acrobatics:
            case Skill::LightArmor:
                return Attribute::Speed;
            case Skill::Security:
            case Skill::Sneak:
            case Skill::Marksman:
                return Attribute::Agility;
            case Skill::Mercantile:
            case Skill::Speechcraft:
            case Skill::Illusion:
                return Attribute::Personality;
            case Skill::Alchemy:
            case Skill::Conjuration:
            case Skill::Mysticism:
                return Attribute::Intelligence;
            case Skill::Alteration:
            case Skill::Destruction:
            case Skill::Restoration:
                return Attribute::Willpower;
        }
        return Attribute::Luck; // unreachable; Luck governs nothing
    }

    Specialization specializationOf(Skill skill)
    {
        switch (skill)
        {
            case Skill::Armorer:
            case Skill::Athletics:
            case Skill::Blade:
            case Skill::Block:
            case Skill::Blunt:
            case Skill::HandToHand:
            case Skill::HeavyArmor:
                return Specialization::Combat;
            case Skill::Alchemy:
            case Skill::Alteration:
            case Skill::Conjuration:
            case Skill::Destruction:
            case Skill::Illusion:
            case Skill::Mysticism:
            case Skill::Restoration:
                return Specialization::Magic;
            case Skill::Acrobatics:
            case Skill::LightArmor:
            case Skill::Marksman:
            case Skill::Mercantile:
            case Skill::Security:
            case Skill::Sneak:
            case Skill::Speechcraft:
                return Specialization::Stealth;
        }
        return Specialization::Combat;
    }

    bool skillIgnoresLuck(Skill skill)
    {
        return skill == Skill::Acrobatics || skill == Skill::Athletics || skill == Skill::Speechcraft;
    }

    std::int32_t effectiveSkillValue(std::int32_t baseValue, Skill skill, std::int32_t luck)
    {
        const std::int32_t base = clampInt(baseValue, kSkillMin, kSkillMax);
        if (skillIgnoresLuck(skill))
            return base;
        const float adjusted = static_cast<float>(base)
            + kLuckSkillFactor * static_cast<float>(luck - kLuckSkillThreshold);
        return clampInt(static_cast<std::int32_t>(std::floor(adjusted)), kSkillMin, kSkillMax);
    }

    SkillRank skillRank(std::int32_t value)
    {
        if (value >= 100)
            return SkillRank::Master;
        if (value >= 75)
            return SkillRank::Expert;
        if (value >= 50)
            return SkillRank::Journeyman;
        if (value >= 25)
            return SkillRank::Apprentice;
        return SkillRank::Novice;
    }

    std::string_view skillRankName(SkillRank rank)
    {
        switch (rank)
        {
            case SkillRank::Novice: return "Novice";
            case SkillRank::Apprentice: return "Apprentice";
            case SkillRank::Journeyman: return "Journeyman";
            case SkillRank::Expert: return "Expert";
            case SkillRank::Master: return "Master";
        }
        return "Unknown";
    }

    /* ============ class ============ */

    bool ClassDefinition::isMajorSkill(Skill skill) const noexcept
    {
        for (const Skill major : mMajorSkills)
            if (major == skill)
                return true;
        return false;
    }

    bool ClassDefinition::isInSpecialization(Skill skill) const noexcept
    {
        return specializationOf(skill) == mSpecialization;
    }

    /* ============ attributes ============ */

    std::int32_t Attributes::operator[](Attribute attribute) const noexcept
    {
        const std::size_t index = static_cast<std::size_t>(attribute);
        return index < kAttributeCount ? mValues[index] : 0;
    }

    std::int32_t& Attributes::operator[](Attribute attribute) noexcept
    {
        const std::size_t index = static_cast<std::size_t>(attribute);
        return mValues[index < kAttributeCount ? index : 0];
    }

    void Attributes::set(Attribute attribute, std::int32_t value) noexcept
    {
        (*this)[attribute] = clampInt(value, kAttributeMin, kAttributeMax);
    }

    /* ============ leveling ============ */

    std::int32_t attributeBonusForSkillIncreases(std::int32_t increases)
    {
        if (increases >= 10)
            return 5;
        if (increases >= 8)
            return 4;
        if (increases >= 5)
            return 3;
        if (increases >= 1)
            return 2;
        return 1;
    }

    ActorStats::ActorStats()
        : ActorStats(0, 0, 0)
    {
        mSkills.fill(kSkillStartingValue);
    }

    ActorStats::ActorStats(std::int32_t initialMaxHealth, std::int32_t initialMaxMagicka,
        std::int32_t initialMaxFatigue)
        : mInitialMaxHealth(initialMaxHealth)
        , mInitialMaxMagicka(initialMaxMagicka)
        , mInitialMaxFatigue(initialMaxFatigue)
    {
        mSkills.fill(kSkillStartingValue);
    }

    void ActorStats::setClass(const ClassDefinition& cls)
    {
        mOwnedClass = cls;
        mClass = &mOwnedClass;
    }

    const ClassDefinition* ActorStats::getClass() const noexcept
    {
        return mClass;
    }

    void ActorStats::initializeFromClass()
    {
        mSkills.fill(kSkillStartingValue);
        if (!mClass)
            return;
        for (const Skill major : mClass->mMajorSkills)
        {
            const std::size_t index = static_cast<std::size_t>(major);
            if (index >= kSkillCount)
                continue;
            const bool inSpecialization = mClass->isInSpecialization(major);
            mSkills[index] = kSkillMajorStartingValue + (inSpecialization ? kSkillMajorSpecializationBonus : 0);
        }
    }

    std::int32_t ActorStats::getAttribute(Attribute attribute) const noexcept
    {
        return mAttributes[attribute];
    }

    void ActorStats::setAttribute(Attribute attribute, std::int32_t value) noexcept
    {
        mAttributes.set(attribute, value);
    }

    std::int32_t ActorStats::getMaxMagicka() const noexcept
    {
        // +1 magicka per point of Intelligence (base values may be boosted
        // above 100, so the result is not capped).
        return mInitialMaxMagicka + mAttributes[Attribute::Intelligence];
    }

    std::int32_t ActorStats::getMaxFatigue() const noexcept
    {
        // +1 fatigue per Strength, Endurance, Agility and Willpower on top of the base pool.
        return mInitialMaxFatigue + mAttributes[Attribute::Strength] + mAttributes[Attribute::Endurance]
            + mAttributes[Attribute::Agility] + mAttributes[Attribute::Willpower];
    }

    std::int32_t ActorStats::getMaxEncumbrance() const noexcept
    {
        // 5 points of carrying capacity per point of Strength.
        return 5 * mAttributes[Attribute::Strength];
    }

    std::int32_t ActorStats::getMaxHealth() const noexcept
    {
        // Initial health plus 10% of Endurance per level up.
        return mInitialMaxHealth + static_cast<std::int32_t>(std::floor(mHealthFromLevelUps));
    }

    std::int32_t ActorStats::getSkill(Skill skill) const noexcept
    {
        const std::size_t index = static_cast<std::size_t>(skill);
        return index < kSkillCount ? mSkills[index] : 0;
    }

    void ActorStats::setSkill(Skill skill, std::int32_t value) noexcept
    {
        const std::size_t index = static_cast<std::size_t>(skill);
        if (index < kSkillCount)
            mSkills[index] = clampInt(value, kSkillMin, kSkillMax);
    }

    std::int32_t ActorStats::getEffectiveSkill(Skill skill) const noexcept
    {
        return effectiveSkillValue(getSkill(skill), skill, getAttribute(Attribute::Luck));
    }

    bool ActorStats::raiseSkill(Skill skill, std::int32_t amount)
    {
        if (amount <= 0)
            return false;
        const std::size_t index = static_cast<std::size_t>(skill);
        if (index >= kSkillCount)
            return false;

        mSkills[index] = clampInt(mSkills[index] + amount, kSkillMin, kSkillMax);

        // Track skill increases per governing attribute (Luck governs none).
        const Attribute governing = governingAttribute(skill);
        const std::size_t attrIndex = static_cast<std::size_t>(governing);
        if (governing != Attribute::Luck && attrIndex < kAttributeCount)
            mAttributeSkillIncreases[attrIndex] = clampInt(mAttributeSkillIncreases[attrIndex] + amount, 0,
                std::numeric_limits<std::int32_t>::max());

        if (mClass && mClass->isMajorSkill(skill))
            mMajorSkillIncreases = clampInt(mMajorSkillIncreases + amount, 0, std::numeric_limits<std::int32_t>::max());

        return mSkills[index] >= kSkillMax;
    }

    std::int32_t ActorStats::getMajorSkillIncreases() const noexcept
    {
        return mMajorSkillIncreases;
    }

    std::int32_t ActorStats::skillIncreasesForAttribute(Attribute attribute) const noexcept
    {
        const std::size_t index = static_cast<std::size_t>(attribute);
        return index < kAttributeCount ? mAttributeSkillIncreases[index] : 0;
    }

    bool ActorStats::trainSkill(Skill skill)
    {
        if (!canTrain())
            return false;
        // A training session grants one skill point (clamped by raiseSkill); consuming
        // one of the limited per-level training slots does not depend on where the skill
        // ends up. raiseSkill's return value only reports the skill reaching the cap and
        // carries no failure semantics here.
        raiseSkill(skill, 1);
        ++mTrainingsUsed;
        return true;
    }

    std::int32_t ActorStats::attributeBonusForLevelUp(Attribute attribute) const noexcept
    {
        if (attribute == Attribute::Luck)
            return 0;
        return attributeBonusForSkillIncreases(skillIncreasesForAttribute(attribute));
    }

    bool ActorStats::levelUp(const std::array<Attribute, 3>& chosenAttributes)
    {
        if (!isLevelUpReady())
            return false;

        // Three distinct, governable attributes are required; Luck cannot be chosen.
        bool seen[kAttributeCount] = {};
        for (const Attribute attribute : chosenAttributes)
        {
            if (attribute == Attribute::Luck)
                return false;
            const std::size_t index = static_cast<std::size_t>(attribute);
            if (index >= kAttributeCount || seen[index])
                return false;
            seen[index] = true;
        }

        for (const Attribute attribute : chosenAttributes)
        {
            const std::int32_t bonus = attributeBonusForLevelUp(attribute);
            mAttributes[attribute] = clampInt(mAttributes[attribute] + bonus, kAttributeMin, kAttributeMax);
        }

        // Health gains 10% of the (now updated) Endurance.
        mHealthFromLevelUps += static_cast<float>(mAttributes[Attribute::Endurance]) * kHealthGainPerEndurance;

        mMajorSkillIncreases -= kMajorSkillIncreasesPerLevelUp;
        std::fill(mAttributeSkillIncreases.begin(), mAttributeSkillIncreases.end(), 0);
        mTrainingsUsed = 0;
        ++mLevel;
        return true;
    }

    AttributesSnapshot ActorStats::snapshot() const noexcept
    {
        AttributesSnapshot snapshot;
        snapshot.mValues = mAttributes.mValues;
        snapshot.mAttributeSkillIncreases = mAttributeSkillIncreases;
        std::copy(mSkills.begin(), mSkills.end(), snapshot.mBaseSkills.begin());
        snapshot.mMajorSkillIncreases = mMajorSkillIncreases;
        snapshot.mTrainingsUsed = mTrainingsUsed;
        snapshot.mLevel = mLevel;
        snapshot.mHealthFromLevelUps = mHealthFromLevelUps;
        return snapshot;
    }

    void ActorStats::restoreFromSnapshot(const AttributesSnapshot& snapshot) noexcept
    {
        mAttributes.mValues = snapshot.mValues;
        mAttributeSkillIncreases = snapshot.mAttributeSkillIncreases;
        std::copy(snapshot.mBaseSkills.begin(), snapshot.mBaseSkills.end(), mSkills.begin());
        mMajorSkillIncreases = snapshot.mMajorSkillIncreases;
        mTrainingsUsed = snapshot.mTrainingsUsed;
        mLevel = snapshot.mLevel;
        mHealthFromLevelUps = snapshot.mHealthFromLevelUps;
    }

    /* ============ magic schools ============ */

    Skill schoolSkill(MagicSchool school)
    {
        switch (school)
        {
            case MagicSchool::Alteration: return Skill::Alteration;
            case MagicSchool::Conjuration: return Skill::Conjuration;
            case MagicSchool::Destruction: return Skill::Destruction;
            case MagicSchool::Illusion: return Skill::Illusion;
            case MagicSchool::Mysticism: return Skill::Mysticism;
            case MagicSchool::Restoration: return Skill::Restoration;
        }
        return Skill::Alteration;
    }

    std::string_view schoolName(MagicSchool school)
    {
        switch (school)
        {
            case MagicSchool::Alteration: return "Alteration";
            case MagicSchool::Conjuration: return "Conjuration";
            case MagicSchool::Destruction: return "Destruction";
            case MagicSchool::Illusion: return "Illusion";
            case MagicSchool::Mysticism: return "Mysticism";
            case MagicSchool::Restoration: return "Restoration";
        }
        return "Unknown";
    }

    MagicSchool skillToSchool(Skill skill)
    {
        switch (skill)
        {
            case Skill::Alteration: return MagicSchool::Alteration;
            case Skill::Conjuration: return MagicSchool::Conjuration;
            case Skill::Destruction: return MagicSchool::Destruction;
            case Skill::Illusion: return MagicSchool::Illusion;
            case Skill::Mysticism: return MagicSchool::Mysticism;
            case Skill::Restoration: return MagicSchool::Restoration;
            default: break;
        }
        return MagicSchool::Alteration;
    }

    /* ============ spell effect table ============ */

    const SpellEffectInfo* findSpellEffectInfo(std::uint32_t effectId) noexcept
    {
        for (const SpellEffectInfo& info : sSpellEffects)
            if (info.mId == effectId)
                return &info;
        return nullptr;
    }

    std::size_t numSpellEffects() noexcept
    {
        return sSpellEffects.size();
    }

    const SpellEffectInfo& spellEffectInfoAt(std::size_t index) noexcept
    {
        return sSpellEffects.at(index);
    }

    /* ============ spell cost formulas ============ */

    namespace
    {
        // magnitude^1.28 with the magnitude clamped to at least 1
        float spellMagnitudeFactor(std::int32_t magnitude) noexcept
        {
            const float m = static_cast<float>(std::max(1, magnitude));
            return std::pow(m, 1.28f);
        }

        float spellAreaFactor(std::int32_t area) noexcept
        {
            const float a = static_cast<float>(area) * 0.15f;
            return std::max(1.f, a);
        }

        float spellTargetMultiplier(SpellRange range) noexcept
        {
            return range == SpellRange::Target ? 1.5f : 1.f;
        }

        float skillCostMultiplier(std::int32_t effectiveSchoolSkill) noexcept
        {
            const std::int32_t skill = clampInt(effectiveSchoolSkill, 0, 100);
            return 1.4f - 0.012f * static_cast<float>(skill);
        }

        // Effect parameters for costing: magnitude-only effects (Dispel) have duration 1;
        // duration-only effects have magnitude 1.
        std::int32_t effectiveMagnitude(const SpellEffect& effect) noexcept
        {
            if (const SpellEffectInfo* info = findSpellEffectInfo(effect.mEffectId))
                if (info->mDurationOnly)
                    return 1;
            return std::max(1, effect.mMaxMagnitude);
        }

        std::int32_t effectiveDuration(const SpellEffect& effect) noexcept
        {
            if (const SpellEffectInfo* info = findSpellEffectInfo(effect.mEffectId))
                if (info->mMagnitudeOnly)
                    return 1;
            return std::max(1, effect.mDuration);
        }
    } // namespace

    float effectBaseMagickaCost(const SpellEffect& effect)
    {
        const SpellEffectInfo* info = findSpellEffectInfo(effect.mEffectId);
        if (!info)
            return 0.f;
        const float baseCost = std::max(0.f, info->mBaseCost) / 10.f;
        return baseCost * spellMagnitudeFactor(effectiveMagnitude(effect))
            * static_cast<float>(effectiveDuration(effect)) * spellAreaFactor(effect.mArea)
            * spellTargetMultiplier(effect.mRange);
    }

    float effectMagickaCost(const SpellEffect& effect, std::int32_t effectiveSchoolSkill)
    {
        return effectBaseMagickaCost(effect) * skillCostMultiplier(effectiveSchoolSkill);
    }

    float spellMagickaCost(const Spell& spell, const SchoolSkills& schoolSkills)
    {
        float total = 0.f;
        for (const SpellEffect& effect : spell.mEffects)
        {
            const SpellEffectInfo* info = findSpellEffectInfo(effect.mEffectId);
            if (!info)
                continue;
            const std::size_t schoolIndex = static_cast<std::size_t>(info->mSchool);
            const std::int32_t schoolSkill = schoolIndex < kMagicSchoolCount ? schoolSkills[schoolIndex] : 0;
            total += effectMagickaCost(effect, schoolSkill);
        }
        return total;
    }

    float spellBaseMagickaCost(const Spell& spell)
    {
        float total = 0.f;
        for (const SpellEffect& effect : spell.mEffects)
            total += effectBaseMagickaCost(effect);
        return total;
    }

    MagicSchool primarySpellSchool(const Spell& spell)
    {
        MagicSchool best = MagicSchool::Alteration;
        float bestCost = -1.f;
        for (const SpellEffect& effect : spell.mEffects)
        {
            const SpellEffectInfo* info = findSpellEffectInfo(effect.mEffectId);
            if (!info)
                continue;
            const float cost = effectBaseMagickaCost(effect);
            if (cost > bestCost)
            {
                bestCost = cost;
                best = info->mSchool;
            }
        }
        return best;
    }

    std::int32_t spellRequiredSkillForBaseCost(float unadjustedBaseCost)
    {
        if (unadjustedBaseCost < 26.f)
            return 0;
        if (unadjustedBaseCost < 63.f)
            return 25;
        if (unadjustedBaseCost < 150.f)
            return 50;
        if (unadjustedBaseCost < 400.f)
            return 75;
        return 100;
    }

    SpellRequirement spellSkillRequirement(const Spell& spell)
    {
        SpellRequirement requirement;
        requirement.mSchool = primarySpellSchool(spell);
        requirement.mRequiredSkill = spellRequiredSkillForBaseCost(spellBaseMagickaCost(spell));
        return requirement;
    }

    std::int32_t spellGoldCost(const Spell& spell, const SchoolSkills& schoolSkills)
    {
        return static_cast<std::int32_t>(std::floor(3.f * spellMagickaCost(spell, schoolSkills)));
    }

    bool canCastSpell(const Spell& spell, const SchoolSkills& schoolSkills, std::int32_t currentMagicka)
    {
        if (spell.mEffects.empty())
            return false;
        const SpellRequirement requirement = spellSkillRequirement(spell);
        const std::size_t schoolIndex = static_cast<std::size_t>(requirement.mSchool);
        const std::int32_t schoolSkill = schoolIndex < kMagicSchoolCount ? schoolSkills[schoolIndex] : 0;
        if (schoolSkill < requirement.mRequiredSkill)
            return false;
        return spellMagickaCost(spell, schoolSkills) <= static_cast<float>(currentMagicka);
    }

    SchoolSkills effectiveSchoolSkills(const ActorStats& stats)
    {
        SchoolSkills skills{};
        skills[static_cast<std::size_t>(MagicSchool::Alteration)] = stats.getEffectiveSkill(Skill::Alteration);
        skills[static_cast<std::size_t>(MagicSchool::Conjuration)] = stats.getEffectiveSkill(Skill::Conjuration);
        skills[static_cast<std::size_t>(MagicSchool::Destruction)] = stats.getEffectiveSkill(Skill::Destruction);
        skills[static_cast<std::size_t>(MagicSchool::Illusion)] = stats.getEffectiveSkill(Skill::Illusion);
        skills[static_cast<std::size_t>(MagicSchool::Mysticism)] = stats.getEffectiveSkill(Skill::Mysticism);
        skills[static_cast<std::size_t>(MagicSchool::Restoration)] = stats.getEffectiveSkill(Skill::Restoration);
        return skills;
    }

    /* ============ soul gems & enchantments ============ */

    std::int32_t soulGemCapacity(SoulGemType type) noexcept
    {
        switch (type)
        {
            case SoulGemType::Petty: return 150;
            case SoulGemType::Lesser: return 300;
            case SoulGemType::Common: return 800;
            case SoulGemType::Greater: return 1200;
            case SoulGemType::Grand: return 1600;
            case SoulGemType::Black: return 1600;
        }
        return 0;
    }

    std::int32_t soulGemLevel(SoulGemType type) noexcept
    {
        switch (type)
        {
            case SoulGemType::Petty: return 1;
            case SoulGemType::Lesser: return 2;
            case SoulGemType::Common: return 3;
            case SoulGemType::Greater: return 4;
            case SoulGemType::Grand: return 5;
            case SoulGemType::Black: return 5;
        }
        return 0;
    }

    std::string_view soulGemName(SoulGemType type) noexcept
    {
        switch (type)
        {
            case SoulGemType::Petty: return "Petty Soul Gem";
            case SoulGemType::Lesser: return "Lesser Soul Gem";
            case SoulGemType::Common: return "Common Soul Gem";
            case SoulGemType::Greater: return "Greater Soul Gem";
            case SoulGemType::Grand: return "Grand Soul Gem";
            case SoulGemType::Black: return "Black Soul Gem";
        }
        return "Unknown";
    }

    namespace
    {
        const ConstantEnchantRow* findConstantEnchantRow(std::uint32_t effectId) noexcept
        {
            for (const ConstantEnchantRow& row : sConstantMagnitudes)
                if (row.mEffectId == effectId)
                    return &row;
            return nullptr;
        }
    } // namespace

    std::int32_t constantEffectEnchantmentMagnitude(std::uint32_t effectId, std::int32_t soulLevel)
    {
        const ConstantEnchantRow* row = findConstantEnchantRow(effectId);
        if (!row)
            return 0;
        if (row->mMagnitudes[0] == FLAT)
            return 5; // Night-Eye / Water Breathing / Water Walking use a fixed magnitude of 5
        const std::int32_t level = clampInt(soulLevel, 1, 5);
        return row->mMagnitudes[level - 1];
    }

    std::int32_t constantEffectEnchantmentValueBonus(std::uint32_t effectId, std::int32_t soulLevel)
    {
        const SpellEffectInfo* info = findSpellEffectInfo(effectId);
        if (!info)
            return 0;
        const float magnitude = static_cast<float>(constantEffectEnchantmentMagnitude(effectId, soulLevel));
        return static_cast<std::int32_t>(std::floor(magnitude * info->mBarterFactor));
    }

    std::int32_t weaponEnchantmentChargeCost(const Enchantment& enchantment)
    {
        float total = 0.f;
        for (const SpellEffect& effect : enchantment.mEffects)
            total += effectBaseMagickaCost(effect); // unadjusted, as built-in weapon enchantments are
        return static_cast<std::int32_t>(std::floor(total));
    }

    std::int32_t weaponEnchantmentUses(std::int32_t charge, std::int32_t costPerUse)
    {
        if (costPerUse <= 0 || charge <= 0)
            return 0;
        return charge / costPerUse;
    }

    float enchantedWeaponValueBonus(std::int32_t enchantmentCost, std::int32_t remainingCharge)
    {
        return kEnchantedWeaponValueFactor * static_cast<float>(enchantmentCost + std::max(0, remainingCharge));
    }

    /* ============ items & inventory ============ */

    std::string_view itemTypeName(ItemType type) noexcept
    {
        switch (type)
        {
            case ItemType::Misc: return "Misc";
            case ItemType::Weapon: return "Weapon";
            case ItemType::Armor: return "Armor";
            case ItemType::Clothing: return "Clothing";
            case ItemType::Ingredient: return "Ingredient";
            case ItemType::Potion: return "Potion";
            case ItemType::Key: return "Key";
            case ItemType::Book: return "Book";
        }
        return "Unknown";
    }

    Inventory::Inventory(const std::unordered_map<std::uint32_t, ItemDefinition>& definitions)
        : mDefinitions(&definitions)
    {
    }

    const ItemDefinition* Inventory::getDefinition(std::uint32_t formId) const noexcept
    {
        if (!mDefinitions)
            return nullptr;
        const auto it = mDefinitions->find(formId);
        return it != mDefinitions->end() ? &it->second : nullptr;
    }

    std::int32_t Inventory::getItemCount(std::uint32_t formId) const noexcept
    {
        std::int32_t total = 0;
        for (const ItemInstance& item : mItems)
            if (item.mFormId == formId)
                total += item.mCount;
        return total;
    }

    std::int32_t Inventory::addItem(std::uint32_t formId, std::int32_t count, bool stolen)
    {
        if (count <= 0)
            return 0;
        // Merge into stacks of the same formId that are still pristine (full
        // condition, no charge, same stolen flag).
        for (ItemInstance& item : mItems)
        {
            const bool mergeable = item.mCondition >= 1.f && item.mCharge == 0 && item.mMaxCharge == 0
                && item.mStolen == stolen;
            if (item.mFormId == formId && mergeable)
            {
                item.mCount += count;
                return count;
            }
        }
        ItemInstance instance;
        instance.mFormId = formId;
        instance.mCount = count;
        instance.mStolen = stolen;
        mItems.push_back(instance);
        return count;
    }

    bool Inventory::removeItem(std::uint32_t formId, std::int32_t count)
    {
        if (count <= 0 || getItemCount(formId) < count)
            return false;
        std::int32_t remaining = count;
        for (std::size_t i = 0; i < mItems.size() && remaining > 0;)
        {
            ItemInstance& item = mItems[i];
            if (item.mFormId != formId)
            {
                ++i;
                continue;
            }
            const std::int32_t taken = std::min(item.mCount, remaining);
            item.mCount -= taken;
            remaining -= taken;
            if (item.mCount <= 0)
                mItems.erase(mItems.begin() + static_cast<std::ptrdiff_t>(i));
            else
                ++i;
        }
        return true;
    }

    bool Inventory::hasItem(std::uint32_t formId, std::int32_t count) const noexcept
    {
        return getItemCount(formId) >= count;
    }

    float Inventory::getTotalWeight() const noexcept
    {
        float total = 0.f;
        for (const ItemInstance& item : mItems)
        {
            if (const ItemDefinition* def = getDefinition(item.mFormId))
                total += def->mWeight * static_cast<float>(item.mCount);
        }
        return total;
    }

    bool Inventory::canCarry(float capacity) const noexcept
    {
        return getTotalWeight() <= capacity;
    }

    float Inventory::getFreeCapacity(float capacity) const noexcept
    {
        return std::max(0.f, capacity - getTotalWeight());
    }

    std::int32_t Inventory::getTotalValue() const noexcept
    {
        std::int64_t total = 0;
        for (const ItemInstance& item : mItems)
        {
            if (const ItemDefinition* def = getDefinition(item.mFormId))
                total += static_cast<std::int64_t>(def->mValue) * item.mCount;
        }
        return static_cast<std::int32_t>(std::min<std::int64_t>(total, std::numeric_limits<std::int32_t>::max()));
    }

    void Inventory::clearStolenFlag(std::uint32_t formId) noexcept
    {
        for (ItemInstance& item : mItems)
            if (item.mFormId == formId)
                item.mStolen = false;
    }

    void Inventory::applyEnchantment(std::uint32_t formId) noexcept
    {
        for (ItemInstance& item : mItems)
            if (item.mFormId == formId)
            {
                item.mCondition = 1.f; // enchanting repair always sets the item to 100%
                item.mStolen = false; // enchanted items are no longer treated as stolen
            }
    }

    void Inventory::setItemCharge(std::uint32_t formId, std::int32_t charge, std::int32_t maxCharge) noexcept
    {
        for (ItemInstance& item : mItems)
            if (item.mFormId == formId)
            {
                item.mCharge = charge;
                item.mMaxCharge = maxCharge;
            }
    }

    std::int32_t Inventory::repairItem(std::uint32_t formId, std::int32_t effectiveArmorer)
    {
        const ItemDefinition* def = getDefinition(formId);
        if (!def || def->mMaxHealth <= 0)
            return 0;
        if (def->mIsMagicItem && !canRepairMagicItems(effectiveArmorer))
            return 0;

        const float maximumCondition = maxRepairCondition(effectiveArmorer);
        const float maximumHealth = static_cast<float>(def->mMaxHealth) * maximumCondition;
        const std::int32_t repairable = armorerHealthRepaired(effectiveArmorer);

        for (ItemInstance& item : mItems)
        {
            if (item.mFormId != formId)
                continue;
            const float currentHealth = item.mCondition * static_cast<float>(def->mMaxHealth);
            const float missing = maximumHealth - currentHealth;
            if (missing <= 0.f)
                return 0;
            const float restored = std::min(missing, static_cast<float>(repairable));
            item.mCondition = (currentHealth + restored) / static_cast<float>(def->mMaxHealth);
            return static_cast<std::int32_t>(std::floor(restored));
        }
        return 0;
    }

    /* ============ armorer repair ============ */

    std::int32_t armorerHealthRepaired(std::int32_t effectiveArmorer) noexcept
    {
        return std::max(0, effectiveArmorer) + kArmorerRepairBase;
    }

    float maxRepairCondition(std::int32_t effectiveArmorer) noexcept
    {
        return effectiveArmorer >= 75 ? 1.25f : 1.0f;
    }

    bool canRepairMagicItems(std::int32_t effectiveArmorer) noexcept
    {
        return effectiveArmorer >= 50;
    }

    bool repairHammerBreaks(std::int32_t effectiveArmorer, float roll, std::int32_t baseBreakChancePercent) noexcept
    {
        if (effectiveArmorer >= 100)
            return false; // Master Armorer never breaks a hammer
        float chance = static_cast<float>(std::max(0, baseBreakChancePercent)) / 100.f;
        if (effectiveArmorer >= 25)
            chance *= 0.5f; // Apprentice Armorer hammers last twice as long
        return roll >= 0.f && roll < chance;
    }

    /* ============ alchemy ============ */

    float apparatusStrength(ApparatusQuality quality) noexcept
    {
        switch (quality)
        {
            case ApparatusQuality::Novice: return 0.1f;
            case ApparatusQuality::Apprentice: return 0.25f;
            case ApparatusQuality::Journeyman: return 0.5f;
            case ApparatusQuality::Expert: return 0.75f;
            case ApparatusQuality::Master: return 1.0f;
        }
        return 0.1f;
    }

    std::int32_t effectiveAlchemyValue(std::int32_t baseAlchemy, std::int32_t baseLuck) noexcept
    {
        const float adjusted = static_cast<float>(baseAlchemy) + 0.4f * static_cast<float>(baseLuck - 50);
        return clampInt(static_cast<std::int32_t>(std::floor(adjusted)), 0, 100);
    }

    std::int32_t alchemyIdentifiedEffects(std::int32_t alchemyValue) noexcept
    {
        if (alchemyValue >= 75)
            return 4;
        if (alchemyValue >= 50)
            return 3;
        if (alchemyValue >= 25)
            return 2;
        return 1;
    }

    bool alchemyCanBrewSingleIngredient(std::int32_t alchemyValue) noexcept
    {
        return alchemyValue >= 100;
    }

    namespace
    {
        // Compute the potion effect parameters following the formulas in
        // https://en.uesp.net/wiki/Oblivion:Alchemy#Calculating_Potion_Strengths.
        PotionEffect computePotionEffect(const SpellEffectInfo& info, bool mixtureIsPoison,
            std::int32_t effectiveAlchemy, const AlchemyApparatus& apparatus) noexcept
        {
            const float mortar = apparatusStrength(apparatus.mMortar) * kAlchemyMortarScale;
            const float raw = static_cast<float>(effectiveAlchemy) + mortar;
            const float calc = calcinatorStrength(apparatus);
            const float retort = retortStrength(apparatus);
            const float alembic = alembicStrength(apparatus);
            const bool isNegativeEffect = info.mHarmful;
            // An Alembic only ever helps the negative side effects of a *potion*;
            // poisons (all-negative brews) ignore its strength.
            const bool alembicApplies = isNegativeEffect && !mixtureIsPoison && alembic > 0.f;
            const bool retortApplies = !isNegativeEffect && retort > 0.f;
            const bool calcAndRetort = retortApplies && calc > 0.f;

            PotionEffect effect;
            effect.mEffectId = info.mId;

            if (info.mDurationOnly)
            {
                effect.mMagnitude = 1;
                // Base_Dur = (Effective_Alchemy + Mortar*25) / (BaseCost/10)
                const float baseCost = info.mBaseCost > 0.f ? info.mBaseCost / 10.f : 1.f;
                float duration = raw / baseCost;
                float factor = 1.f + 0.25f * calc;
                if (retortApplies)
                    factor += 0.35f * retort;
                if (alembicApplies)
                    factor -= 2.f * alembic;
                effect.mDuration = roundMinOne(duration * factor);
                return effect;
            }

            if (info.mMagnitudeOnly)
            {
                effect.mDuration = 1;
                // Base_Mag = [ (Effective_Alchemy + Mortar*25) / (BaseCost/10) ] ^ (1/1.28)
                const float baseCost = info.mBaseCost > 0.f ? info.mBaseCost / 10.f : 1.f;
                const float baseMagnitude = std::pow(raw / baseCost, 1.f / 1.28f);
                float magnitude;
                if (calcAndRetort)
                    // Dispel peculiarity: the calcinator and retort strengths are multiplied together
                    magnitude = baseMagnitude * (1.f + 0.3f * calc * 0.5f * retort);
                else
                    magnitude = baseMagnitude * (1.f + 0.3f * calc + (retortApplies ? 0.5f * retort : 0.f));
                effect.mMagnitude = roundMinOne(magnitude);
                return effect;
            }

            // "Most effects" (both magnitude and duration variable):
            // Base_Mag = [ (Effective_Alchemy + Mortar*25) / (BaseCost/10 * 4) ] ^ (1/2.28), Base_Dur = 4 * Base_Mag
            const float baseCost = info.mBaseCost > 0.f ? info.mBaseCost / 10.f : 1.f;
            const float baseMagnitude = std::pow(raw / (baseCost * 4.f), 1.f / 2.28f);
            const float baseDuration = 4.f * baseMagnitude;

            if (!isNegativeEffect)
            {
                // Positive effects: a retort adds +50% magnitude / +100% duration;
                // when both a calcinator and a retort are present, the calcinator contributes +140% instead of +35%.
                const float calcFactor = calcAndRetort ? 1.4f * calc : 0.35f * calc;
                const float magnitude = baseMagnitude * (1.f + calcFactor + (retortApplies ? 0.5f * retort : 0.f));
                const float duration = baseDuration * (1.f + calcFactor + (retortApplies ? 1.f * retort : 0.f));
                effect.mMagnitude = roundMinOne(magnitude);
                effect.mDuration = roundMinOne(duration);
                return effect;
            }

            // Negative effects: retorts do nothing. Alembic handling follows the two
            // exceptions for "most effects": without one, the calcinator contributes
            // 0.35x once; with one, the calcinator is counted twice.
            float factor = 1.f + 0.35f * calc;
            if (apparatus.mHasAlembic)
            {
                if (mixtureIsPoison)
                    // Alem_Fac is 0 in poisons, but the calcinator is counted twice
                    factor = (1.f + 0.35f * calc) * (1.f + 0.35f * calc);
                else
                    factor = (1.f + 0.35f * calc) * (1.f + 0.35f * calc - 2.f * alembic);
            }
            effect.mMagnitude = roundMinOne(baseMagnitude * factor);
            effect.mDuration = roundMinOne(baseDuration * factor);
            return effect;
        }
    } // namespace

    std::vector<PotionEffect> brewPotion(const std::vector<AlchemyIngredient>& ingredients,
        std::int32_t baseAlchemy, std::int32_t baseLuck, const AlchemyApparatus& apparatus)
    {
        std::vector<PotionEffect> result;
        if (ingredients.empty())
            return result;

        const std::int32_t effectiveAlchemy = effectiveAlchemyValue(baseAlchemy, baseLuck);
        const bool masterAlchemist = alchemyCanBrewSingleIngredient(baseAlchemy);

        // Single ingredient brewing (Master Alchemist only): only the first
        // identified effect is transferred; nothing else is added.
        if (ingredients.size() == 1)
        {
            if (!masterAlchemist || ingredients[0].mKnownEffects.empty())
                return result;
            const SpellEffectInfo* info = findSpellEffectInfo(ingredients[0].mKnownEffects.front());
            if (!info)
                return result;
            result.push_back(computePotionEffect(*info, info->mHarmful, effectiveAlchemy, apparatus));
            return result;
        }

        // Count occurrences of every identified effect across ingredients.
        std::unordered_map<std::uint32_t, std::int32_t> occurrences;
        for (const AlchemyIngredient& ingredient : ingredients)
            for (const std::uint32_t effectId : ingredient.mKnownEffects)
                ++occurrences[effectId];

        // An effect must appear on at least two ingredients to be brewed.
        std::vector<const SpellEffectInfo*> commonEffects;
        for (const auto& [effectId, count] : occurrences)
        {
            if (count < 2)
                continue;
            if (const SpellEffectInfo* info = findSpellEffectInfo(effectId))
                commonEffects.push_back(info);
        }
        if (commonEffects.empty())
            return result;

        const bool mixtureIsPoison = std::all_of(commonEffects.begin(), commonEffects.end(),
            [](const SpellEffectInfo* info) { return info->mHarmful; });

        result.reserve(commonEffects.size());
        for (const SpellEffectInfo* info : commonEffects)
            result.push_back(computePotionEffect(*info, mixtureIsPoison, effectiveAlchemy, apparatus));
        return result;
    }

    bool isPoison(const std::vector<PotionEffect>& effects) noexcept
    {
        if (effects.empty())
            return false;
        for (const PotionEffect& effect : effects)
            if (const SpellEffectInfo* info = findSpellEffectInfo(effect.mEffectId))
                if (!info->mHarmful)
                    return false;
        return true;
    }

    float potionWeight(const std::vector<AlchemyIngredient>& ingredients) noexcept
    {
        if (ingredients.empty())
            return 0.f;
        float total = 0.f;
        for (const AlchemyIngredient& ingredient : ingredients)
            total += ingredient.mWeight;
        return total / static_cast<float>(ingredients.size());
    }

    std::int32_t potionSalePrice(std::int32_t baseAlchemy, std::int32_t baseLuck, ApparatusQuality mortar) noexcept
    {
        const std::int32_t effectiveAlchemy = effectiveAlchemyValue(baseAlchemy, baseLuck);
        const float raw = static_cast<float>(effectiveAlchemy) + apparatusStrength(mortar) * kAlchemyMortarScale;
        return static_cast<std::int32_t>(std::floor(raw * 0.45f));
    }

    /* ============ Oblivion AI behaviour ============ */

    DispositionTone dispositionTone(std::int32_t disposition) noexcept
    {
        if (disposition <= 20)
            return DispositionTone::Hostile;
        if (disposition >= 70)
            return DispositionTone::Friendly;
        return DispositionTone::Neutral;
    }

    bool shouldInitiateCombat(std::int32_t aggression, std::int32_t disposition) noexcept
    {
        if (aggression >= kBerserkAggression)
            return true; // attacks everyone, regardless of disposition
        if (aggression <= 5)
            return false; // never initiates combat (0: refuses combat entirely; 1-5: fights only in self-defense)
        // Attack when the target's disposition drops more than 5 points below aggression.
        return aggression > disposition + kCombatDispositionMargin;
    }

    bool shouldFleeCombat(std::int32_t confidence, float healthFraction) noexcept
    {
        if (confidence >= 100)
            return false; // never flees
        // Flee once health dropped below (100 - confidence)% of maximum health.
        const float threshold = 1.f - static_cast<float>(std::clamp(confidence, 0, 100)) / 100.f;
        return healthFraction <= threshold;
    }

    bool shouldYield(std::int32_t /*confidence*/, float healthFraction, std::int32_t disposition) noexcept
    {
        // Vanilla surrenders are rare: only when almost dead and not deeply hostile.
        return healthFraction <= kYieldHealthFraction && disposition >= 0;
    }

    bool willReportCrime(std::int32_t responsibility) noexcept
    {
        return responsibility > kCrimeReportResponsibility;
    }

    std::int32_t dispositionModifierFromFame(std::int32_t fame) noexcept
    {
        // +3 per 10 fame points, capped at +20 (reached at 67 fame).
        return std::clamp(fame * 3 / 10, 0, 20);
    }

    std::int32_t dispositionModifierFromInfamy(std::int32_t infamy, std::int32_t responsibility) noexcept
    {
        if (infamy <= 0)
            return 0;
        // Linear rate between responsibility 25 (0.0625/infamy) and 100 (0.5/infamy);
        // extrapolated below 25, which eventually makes infamy attractive again.
        const float rate = 0.0625f + static_cast<float>(responsibility - 25) * (0.4375f / 75.f);
        const int magnitude = static_cast<std::int32_t>(std::floor(static_cast<float>(infamy) * std::fabs(rate)));
        const int clamped = std::clamp(magnitude, 0, 20);
        return rate >= 0.f ? -clamped : clamped;
    }

    BehaviorState chooseCombatBehavior(const AiContext& context) noexcept
    {
        if (shouldFleeCombat(context.mData.mConfidence, context.mHealthFraction))
            return BehaviorState::Flee;
        if (shouldYield(context.mData.mConfidence, context.mHealthFraction, context.mData.mDisposition))
            return BehaviorState::Yield;
        return BehaviorState::Fight;
    }

    AiDecision evaluateAiBehavior(const AiContext& context) noexcept
    {
        AiDecision decision;
        decision.mReportsCrime = context.mCrimeWitnessed && willReportCrime(context.mData.mResponsibility);
        decision.mShouldInitiateCombat = context.mTargetVisible
            && shouldInitiateCombat(context.mData.mAggression, context.mData.mDisposition);

        const bool hostile = context.mTargetIsFoe
            || (context.mTargetVisible && shouldInitiateCombat(context.mData.mAggression, context.mData.mDisposition));
        if (hostile)
        {
            decision.mState = chooseCombatBehavior(context);
            return decision;
        }

        switch (context.mCurrentPackage)
        {
            case PackageType::Wander: decision.mState = BehaviorState::Wander; break;
            case PackageType::Follow: decision.mState = BehaviorState::Follow; break;
            case PackageType::Stay: decision.mState = BehaviorState::Stay; break;
            case PackageType::Travel: decision.mState = BehaviorState::Travel; break;
            case PackageType::Eat: decision.mState = BehaviorState::Eat; break;
            case PackageType::Sleep: decision.mState = BehaviorState::Sleep; break;
            case PackageType::Patrol: decision.mState = BehaviorState::Patrol; break;
            case PackageType::Activate: decision.mState = BehaviorState::Activate; break;
            case PackageType::UseItem: decision.mState = BehaviorState::UseItem; break;
        }
        return decision;
    }

    /* ============ game modes ============ */

    std::string_view gameModeName(GameMode mode) noexcept
    {
        switch (mode)
        {
            case GameMode::Game: return "Game";
            case GameMode::Inventory: return "Inventory";
            case GameMode::Container: return "Container";
            case GameMode::Companion: return "Companion";
            case GameMode::Barter: return "Barter";
            case GameMode::Repair: return "Repair";
            case GameMode::SpellBuying: return "Spell Buying";
            case GameMode::SpellMaking: return "Spell Making";
            case GameMode::Enchanting: return "Enchanting";
            case GameMode::Recharge: return "Recharge";
            case GameMode::Training: return "Training";
            case GameMode::MerchantRepair: return "Merchant Repair";
            case GameMode::Alchemy: return "Alchemy";
            case GameMode::LevelUp: return "Level Up";
            case GameMode::Rest: return "Rest";
        }
        return "Unknown";
    }

    bool MechanicsSystem::requestGameMode(GameMode mode) noexcept
    {
        if (mSwitcher)
        {
            if (!mSwitcher->switchToMode(mode))
                return false;
        }
        mGameMode = mode;
        return true;
    }

    void MechanicsSystem::leaveGameMode() noexcept
    {
        if (mSwitcher)
            mSwitcher->switchToMode(GameMode::Game); // the engine is expected to accept this
        mGameMode = GameMode::Game;
    }
}
