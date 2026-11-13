# Oblivion Support in OpenMW

This document describes OpenMW's support for The Elder Scrolls IV: Oblivion (TES4) and for the
TES4-family formats in general: what is implemented, how to build and configure it, how to use
it, and what the current limitations are.

> **Status: experimental.** Oblivion support is under active development. The content pipeline
> (detection, validation, loading, archive access) is functional and covered by automated tests,
> but gameplay fidelity is not at feature parity with Morrowind. Do not expect a finished,
> plug-and-play Oblivion experience. Treat everything described here as pre-release material
> and report problems you find.

## Legal requirements

OpenMW does not ship any game data. To use the Oblivion support you need a legitimately
owned copy of *The Elder Scrolls IV: Oblivion* (a retail disc or a digital purchase) or
another Oblivion-derived game whose content you own. OpenMW will not work with pirated data,
and Oblivion data files will not work with the Morrowind pipeline or vice versa.

## What "Oblivion support" means in OpenMW

Oblivion plugins and masters (`.esp` / `.esm`) share the `TES4` file signature and the
TES4-family record layout with the other Bethesda games of that generation (Fallout 3,
New Vegas, Skyrim). OpenMW's Oblivion support is built around that shared format:

| Layer | Component | Status |
|-------|-----------|--------|
| Format detection (TES3 vs TES4) | `components/esm/format.hpp`, `components/esm/gameformat.hpp` | implemented |
| Oblivion ESM/ESP parser | `components/files/oblivion/esm.{hpp,cpp}` | implemented |
| Oblivion BSA (version 103) reader | `components/files/oblivion/bsa.{hpp,cpp}` | implemented |
| BSA version dispatch (TES4/FO3/SSE) | `components/bsa/bsafile.{hpp,cpp}`, `components/vfs/bsaarchive.hpp` | implemented |
| Oblivion content loading & validation | `components/oblivion/contentloader.{hpp,cpp}` | implemented |
| Oblivion startup content checks | `components/oblivion/config.{hpp,cpp}`, `apps/openmw/main.cpp` | implemented |
| Oblivion gameplay rules (skills, magic, alchemy, AI) | `components/oblivion/mechanics.{hpp,cpp}` | implemented, **not wired into the engine** |
| Engine record loading (generic TES4-family reader) | `components/esm4/`, `apps/openmw/mwworld/esmstore.cpp` | shared with the TES4-family pipeline |
| Oblivion-specific rendering/gameplay integration | `apps/openmw/mwrender/esm4npcanimation.cpp`, `apps/openmw/mwclass/esm4npc.cpp` | partial |
| Oblivion save games (`.ess`) | — | not supported |
| Oblivion scripts (SCPT records) | — | not executed |
| OpenMW CS support for Oblivion | — | not supported |

In short: OpenMW can detect Oblivion content, validate it, read it, mount its archives and
hand the records to the generic TES4-family reader. Many gameplay systems are not yet
wired up to the Oblivion rules.

## Building

Oblivion support is compiled in by default; there is no separate build option and **no new
external dependency** is required beyond what OpenMW already needs (C++20, ZLIB,
Boost.Program_options, …). The build system changes that were made for Oblivion are:

* `components/CMakeLists.txt` registers two new component directories:
  * `files/oblivion` — the self-contained format readers (`esm`, `bsa`), and
  * `oblivion` — the engine-facing systems (`contentloader`, `mechanics`, `config`).
* `apps/components_tests/CMakeLists.txt` registers the new test suites in the
  `components-tests` target:
  * `apps/components_tests/files/oblivion/test_esm.cpp`
  * `apps/components_tests/files/oblivion/test_bsa.cpp`
  * `tests/components/oblivion/test_contentloader.cpp`
  * `tests/integration/test_oblivion_integration.cpp`
  * `tests/integration/test_oblivion_loading.cpp`
  * `tests/integration/test_oblivion_complete.cpp`
* `files/settings-default.cfg` gained an `[Oblivion]` section; it is packed into
  `defaults.bin` by the existing `pack_resource_file` rule in `CMakeLists.txt`.
* `docs/source/reference/modding/settings/oblivion.rst` documents the `[Oblivion]`
  settings and is referenced from the settings index toctree.

New source files follow the existing naming and formatting conventions (lowercase file
names, `.clang-format`, `add_component_dir` with Unity-build support). The new directories
participate in Unity builds (`OPENMW_UNITY_BUILD`) like any other component.

If you are configuring a fresh build tree, nothing special is required:

```sh
cmake -S . -B build -GNinja -DCMAKE_BUILD_TYPE=Release
cmake --build build
```

See the [build instructions](https://wiki.openmw.org/index.php?title=Development_Environment_Setup)
on the wiki for the full dependency list.

## Running the tests

The Oblivion unit and integration tests are built into the `components-tests` target and
run there together with all other component tests:

```sh
cmake --build build --target components-tests
./build/components-tests --gtest_filter='Oblivion*:FilesOblivion*'
```

The tests are self-contained: they synthesise small Oblivion plugin and BSA files in a
temporary directory, so they do **not** require game data, with one exception — the
`OblivionRealData` suite (`apps/components_tests/files/oblivion/test_oblivion_realdata.cpp`)
runs against your real game files and is **skipped unless you set the environment variable
`OPENMW_TEST_OBLIVION_DATA`** to the path of your Oblivion `Data` directory:

```sh
OPENMW_TEST_OBLIVION_DATA="/path/to/Oblivion/Data" \
    ./build/components-tests --gtest_filter='OblivionRealData.*'
```

It parses the real `Oblivion.esm` through the TES4 reader, validates its masters and opens
all official BSAs. The game data is only read, never modified; without the variable set the
three tests report as `SKIPPED`, which is the expected result on machines without the game.

The self-contained tests cover:

* the Oblivion ESM parser (`test_esm.cpp`),
* the Oblivion BSA reader (`test_bsa.cpp`),
* the content loading and validation pipeline (`test_contentloader.cpp`),
* end-to-end startup behaviour: settings, command line options, the startup content
  check, the content loader and the ESM4 reader working together
  (`test_oblivion_integration.cpp`, `test_oblivion_loading.cpp`),
* the complete Oblivion session as a full system test: launch configuration, content
  check, loading of a synthetic world (world space, exterior and interior cells), the
  streaming reader, and gameplay end-to-end (character creation, leveling, Luck-modified
  skills, spellmaking, enchantment, alchemy, inventory, AI behaviour, snapshot/save
  round-trip and the game mode switcher) (`test_oblivion_complete.cpp`).

## Configuring OpenMW for Oblivion

### Data files

Point `data` at your Oblivion `Data` directory and list the content files in load order
(masters before the plugins that depend on them). A typical minimal `openmw.cfg`:

```ini
data="C:/Games/Oblivion/Data"
content=Oblivion.esm
content=MyPatch.esp
fallback-archive=Oblivion - Meshes.bsa
fallback-archive=Oblivion - Textures - Compressed.bsa
fallback-archive=Oblivion - Sounds.bsa
fallback-archive=Oblivion - Voices1.bsa
fallback-archive=Oblivion - Misc.bsa
```

Notes:

* The load order is exactly the order of the `content=` lines, both on the command line
  and in `openmw.cfg`. Master files (e.g. `Oblivion.esm`) must be listed before any plugin
  that depends on them. `Oblivion.esm` must come first.
* `fallback-archive=` lines mount Oblivion's BSA archives. Archives are registered with
  the virtual file system in the order given; later archives take precedence over earlier
  ones, and loose files take precedence over all BSAs.
* Oblivion BSAs (version 103) are read by the dedicated, validating reader in
  `components/files/oblivion/bsa.{hpp,cpp}`. Fallout 3 (104) and Skyrim SE (105) archives
  keep the generic TES4-family reader.
* `.omwgame`/`.omwaddon`/`.omwscripts` content is handled exactly as before and is not
  affected by Oblivion mode.

### Command line options

Oblivion support adds four options to `openmw` (they are also accepted in `openmw.cfg`):

| Option | Default | Description |
|--------|---------|-------------|
| `--oblivion` | `false` | Force Oblivion (TES4) mode for the session. Content files that are not Oblivion plugins/masters are reported as errors, and Oblivion content is always validated at startup. |
| `--oblivion-validate` | `true` | Validate Oblivion content files at startup (TES4 signature, HEDR version, master availability and load order). |
| `--oblivion-strict-load-order` | `false` | Treat missing or misordered Oblivion master files as fatal errors instead of warnings. |
| `--oblivion-verbose` | `false` | Log detailed information about every Oblivion content file that is loaded (HEDR version, author, masters). |

All four options are booleans and accept the usual `boost::program_options` syntax, e.g.
`--oblivion`, `--oblivion=true`, or `oblivion-verbose=true` in `openmw.cfg`.

### Settings

The `[Oblivion]` section of `settings.cfg` provides file-based fallbacks for the command
line options; a command line or `openmw.cfg` value always takes precedence over the
corresponding setting:

```ini
[Oblivion]
enable = true
validate content = true
strict load order = false
verbose content loading = false
```

| Setting | Default | Meaning |
|---------|---------|---------|
| `enable` | `true` | Master switch for Oblivion (TES4) content support. When disabled, configuring an Oblivion plugin/master is an error at startup. Does not affect Morrowind (TES3) content. |
| `validate content` | `true` | Validate Oblivion content files at startup: the TES4 signature and HEDR version are checked, and every master dependency is checked against the configured content files and the data directories. |
| `strict load order` | `false` | Treat missing or misordered masters as fatal errors instead of warnings. Equivalent to `--oblivion-strict-load-order`. |
| `verbose content loading` | `false` | Log detailed information about every Oblivion content file that is loaded. Equivalent to `--oblivion-verbose`. |

The settings are documented with the rest of the advanced settings in
[`docs/source/reference/modding/settings/oblivion.rst`](source/reference/modding/settings/oblivion.rst)
(rendered in the Sphinx manual as the "Oblivion Settings" page). The defaults live in
`files/settings-default.cfg` and are packed into `defaults.bin` at build time.

### Resolution order

For each of the four settings, the effective value is resolved in this order:

1. the command line / `openmw.cfg` option (if explicitly set), else
2. the `[Oblivion]` section of `settings.cfg`, else
3. the built-in default (`enable=true`, `validate content=true`, `strict load order=false`,
   `verbose content loading=false`).

Forcing Oblivion mode (`--oblivion`) implies content validation, even if
`oblivion-validate=false` or `validate content = false` is configured. If Oblivion support
is disabled (`enable = false`) but not overridden by `--oblivion`, configuring Oblivion
content is a fatal error at startup.

### Startup content check

Before any game data is loaded, the engine checks the configured content files **and**
groundcover files against the Oblivion rules:

* Files with extensions other than `.esm`/`.esp` (e.g. `.omwgame`, `.omwaddon`,
  `.omwscripts`) are ignored.
* `.esm`/`.esp` files are located in the configured data directories; a **missing file is
  always a fatal error**.
* Morrowind (TES3) content is reported as *info* (or as an error when Oblivion mode is
  forced).
* Unreadable or corrupt files are reported as *warnings* (or as errors when Oblivion mode
  is forced).
* TES4-family files that are not recognised as Oblivion (unrecognised HEDR version) are
  reported as *warnings* (or *errors* when Oblivion mode is forced).
* Each Oblivion plugin/master is validated (signature, HEDR version 0.8 or 1.0) and its
  master dependencies are checked against the content list and the data directories.
  Missing or misordered masters are *warnings* by default and *fatal errors* with
  `strict load order` enabled.
* If Oblivion support is disabled (`enable = false` in `[Oblivion]`), configuring an
  Oblivion plugin/master is a fatal error at startup (unless Oblivion mode is forced on
  the command line).
* If any *error* is reported, the engine aborts startup instead of loading a broken
  configuration.

With `verbose content loading = true`, every checked Oblivion plugin logs its HEDR version,
author and master list.

## How it works internally

### Format detection

`ESM::Format` (`components/esm/format.hpp`) distinguishes only the two signatures:
`TES3` (Morrowind) and `TES4` (the whole TES4 family). `ESM::GameFormat`
(`components/esm/gameformat.hpp`) refines that to `TES3`, `TES4`, `FO3`, `FNV`, `TES5`
and `FO4` using the HEDR version and, when it is ambiguous (e.g. `VER_100` is used by both
TES4 and FO4), the header `FormVersion` and the formats of previously-loaded masters:

* `isMorrowindVersion()` — `VER_120` / `VER_130` (1.2/1.3),
* `isOblivionVersion()` — `VER_080` (0.8). Note that `VER_100` (1.0, shared with FO4)
  is only distinguished via the header form version, see `getGameFormat()`.
* Oblivion HEDR versions 0.8 and 1.0 are recognised by `isOblivionHedrVersion()` in
  `components/files/oblivion/esm.hpp`.

### Oblivion ESM parser

`components/files/oblivion/esm.{hpp,cpp}` (`Files::Oblivion::EsmFile`) is a self-contained,
validating reader for Oblivion plugin/master files. It:

* checks the `TES4` signature and the HEDR version on open — not an Oblivion plugin means
  `std::runtime_error`,
* parses the record and group (`GRUP`) hierarchy, including the depth-aware group nesting,
* supports the Oblivion group types (world children, interior/exterior cell blocks and
  sub-blocks, cell children, topic children, persistent/temporary/visible-distant children),
* decodes the `XXXX`-extended sub-record size mechanism,
* validates record and group headers (truncated or corrupt files are rejected),
* exposes records in file order, with per-record-type statistics available through
  `Oblivion::ContentLoader` (`components/oblivion/contentloader.{hpp,cpp}`), which also
  extracts the Oblivion-specific cell (`CELL`) and world space (`WRLD`) data — including
  the exterior-cell → world-space association that is implicit in the group hierarchy —
  and all master dependencies.

The parser is used for validation and for the Oblivion-aware view of the content; the
engine still streams the records through the shared TES4-family reader
(`components/esm4/reader.hpp`). Validation failures before that hand-off are logged (and,
for masters that have not been loaded yet, produce a warning) but the authoritative error
is produced by the ESM4 reader itself.

### Content loading pipeline

1. `apps/openmw/main.cpp` runs the startup content check (`Oblivion::checkContentFiles`,
   `components/oblivion/config.{hpp,cpp}`) over content and groundcover files and aborts
   the launch on errors.
2. `apps/openmw/mwworld/esmloader.cpp` dispatches content files by signature. For TES4
   files it additionally runs `Oblivion::ContentLoader::isOblivionPlugin()` and
   `validate()` (logging the result), then hands the file to `ESM4::Reader` with the
   detected `ESM::GameFormat`.
3. `ESMStore::loadESM4` reads the records into the store, exactly as for the other
   TES4-family games.

### Runtime defaults for TES4-only sessions (`tes4defaults`)

The running engine is still the Morrowind runtime: large parts of it dereference
Morrowind-dialect records (globals, game settings, skills, races, the `Player` NPC) that
Oblivion content does not provide. When a session contains **no TES3 content at all**
(`ESMStore::hasTES3Content()` is false), `ESMStore::setUp()` seeds the mandatory vocabulary
via `apps/openmw/mwworld/tes4defaults.{hpp,cpp}`:

* **Globals** — `World::ensureNeededRecords()` runs for TES4-only sessions too (the same
  mandatory set vanilla OpenMW inserts for old Morrowind masters: `dayspassed`, `gamehour`,
  `timescale`, …).
* **Game settings** — the full vanilla GMST vocabulary (258 float + 89 int + 1174 string
  names, generated from `apps/opencs/model/world/defaultgmsts.cpp`; see
  `tes4defaults_gmsts.hpp`) plus every `openmw.cfg` fallback entry (`FontColor_*`,
  `Movies_*`, …) mirrored into the GMST store so Lua UI scripts resolve them. String GMSTs
  are seeded empty (UI text will be blank until content provides it). Only records missing
  from the store are inserted — real content always wins.
* **Skills** — the 27 Morrowind skill records (`SKIL`), which `NpcStats` and the Lua
  mechanics scripts iterate unconditionally. Oblivion's own `SKIL` records are *not*
  bridged into this store.
* **Player and race** — a minimal synthetic `Player` NPC with a synthetic race
  (`playerrace`). Vanilla Oblivion's player base object exists but is flagged invalid (and
  NPCE records have no esm3 conversion), so without the seed the engine aborts in
  `World::setupPlayer()`. The seeded actor has no usable meshes; starting an actual game
  additionally fails on missing Morrowind body meshes (see Limitations).

Mixed TES3+TES4 sessions are not seeded at all — the Morrowind content provides the
vocabulary.

### BSA archives

Oblivion ships its data in BSA archives with the version word `0x67` (103). Version 103 is
detected in `Bsa::BSAFile::detectVersion()` (`components/bsa/bsafile.cpp`) and reported as
`Bsa::BsaVersion::TES4`. `Bsa::BsaVersion` also gained `FO3` (`0x68`, 104) and `SSE`
(`0x69`, 105) for the later TES4-family archive versions.

`Files::Oblivion::BsaFile` (`components/files/oblivion/bsa.{hpp,cpp}`) implements the
Oblivion flavour of the format: a 36-byte header, 16-byte folder records, 16-byte file
records, the optional folder/file name tables, zlib-compressed members (per-file or by
archive default), and the Oblivion archive and file flags. It is read-only, rejects
Xbox 360 (big-endian) archives and archive versions other than 103 with a descriptive
error, and caches extracted contents in memory (bounded, 16 MiB by default, adjustable
with `setCacheLimit()`). Lookups canonicalise `/` vs `\` and letter case. Extracted
archives are wired into the VFS via `components/vfs/bsaarchive.hpp`; FO3/SSE archives keep
the generic `Bsa::CompressedBSAFile` reader, and the BA2 variants are unchanged.

`bsatool` (`apps/bsatool/bsatool.cpp`) recognises the new BSA version values (`TES4`,
`FO3`, `SSE`) and handles Oblivion archives through the generic TES4-family reader.
Archive invalidation is not supported anywhere; load order is the only mechanism, and
loose files always win over archives.

### NIF meshes

`components/nif/niffile.hpp` provides version classifiers used by the NIF reader and the
scene graph:

* `isMorrowindNIFVersion()` — 4.0.0.2,
* `isOblivionNIFVersion()` — 10.0.1.2 or 20.0.0.5,
* `isGamebryoNIFVersion()` — 20.2.0.7 (Fallout 3/4, New Vegas, Skyrim and derivatives).

The NIF reader already handles Gamebryo versions to a varying degree; Oblivion meshes are
in the same family, but they are only as well supported as the rest of the TES4-family
NIF path.

### Oblivion gameplay rules

`components/oblivion/mechanics.{hpp,cpp}` implements the rule systems of Oblivion that
differ from Morrowind's, in a self-contained, unit-tested component with no engine
dependencies (no UI, no world, no esm3 stores):

* the Oblivion skill system: the 21 skills, their governing attributes, specialisations,
  effective values (including the Luck modifier), skill ranks and the level-up rules
  (10 major-skill increases per level, attribute bonuses from skill use, +10 % of
  Endurance to health per level),
* the magic system: the six schools, the spell effect table, spellmaking cost,
  requirement and gold-value formulas, enchantment charge accounting, constant-effect
  enchantments and soul gems,
* items and crafting: item instances with condition/charge/stolen state, encumbrance,
  alchemy brewing (apparatus, strength formulas, effect identification), Armorer repair
  and soul gem enchanting,
* AI behaviour: aggression/confidence/responsibility driven combat decisions, flee/yield
  rules, disposition modifiers from fame/infamy and the PACK package categories,
* Oblivion game modes, exposed through a small `GameModeSwitcher` interface that a host
  engine maps onto its own mode system.

**Important:** this component is currently exercised by the test suite only. The running
engine still uses the Morrowind mechanics (`apps/openmw/mwmechanics`); nothing in the
game loop consults the Oblivion rules yet. The numeric constants and formulas are taken
from the Construction Set data and the reverse-engineered rules documented on the UESP
Wiki; each formula cites its source page in the code.

## Limitations and known issues

Oblivion support is experimental. The following limitations apply:

### Scope of support

* **Not a finished Oblivion port.** There is no guarantee that any given Oblivion world,
  quest, script or mod will work. What works today, verified against vanilla
  `Oblivion.esm` + the five official BSAs (headless Xvfb/llvmpipe): the startup content
  check passes, all archives mount, the plugin streams into the store, world initialisation
  completes and the engine reaches the Lua main menu and stays there stably (no fatal
  errors). The UI renders with magenta error textures because Morrowind interface art is
  not present in Oblivion archives. Choosing New Game fails at the character preview
  (`Failed to start new game: Resource 'meshes/base_anim.nif' not found`); the engine logs
  the failure and remains running. Gameplay beyond that point is unimplemented.
* **No Morrowind-dialect actor vocabulary.** Oblivion `NPC_`/`CREA`/`RACE`/`NPCS` records are
  parsed by the TES4 reader but have no esm3 conversion: the Morrowind-side stores see only
  the synthetic `Player`/`playerrace` seeds (see "Runtime defaults for TES4-only sessions").
* **Scripts are not executed.** The engine's scripting system is Morrowind's mwscript;
  Oblivion's SCPT records are parsed but never run. Quests, triggers, menus and dialogue
  driven by scripts will not function.
* **Oblivion gameplay rules are not wired in.** See "Oblivion gameplay rules" above; the
  mechanics library is validated by tests, not by play.
* **No Oblivion save game support.** The engine writes/reads its own `.omwsave` format;
  Oblivion `.ess` saves cannot be loaded or saved.
* **OpenMW-CS cannot edit Oblivion plugins.** The Construction Set is Morrowind-specific.
* **The launcher is not Oblivion-aware.** Content files must be configured manually in
  `openmw.cfg`; the launcher will not detect an Oblivion installation or manage its
  load order for you.
* **Archive invalidation is not supported.** Oblivion's archive-invalidation mechanism
  (modified resources shipped as loose files) is not detected automatically; load order is
  the only supported behaviour.
* **Groundcover files are checked by the Oblivion validation too.** Groundcover content is
  Morrowind-dialect by definition, so Oblivion plugins configured as groundcover are an
  unusual setup; be aware that the startup check treats them as regular content.

### Format support

* **Only Oblivion HEDR versions 0.8 and 1.0** are recognised by the Oblivion parser.
  `VER_100` is shared with FO4; `getGameFormat()` disambiguates via the header form
  version and the masters' formats. Files that fail this detection are treated as
  "TES4-family but not Oblivion" and are not validated (a warning is logged).
* **Localized plugins** (`.esm`/`.esp` with the localized flag and string tables, as
  written by Skyrim and later games) are not an Oblivion feature. The Oblivion parser
  tolerates the flag when reading but does not support localized string tables.
* **Xbox 360 archives** (big endian, `ArchiveFlag_Xbox360`) are rejected with a
  descriptive error instead of being mis-parsed.
* **Archive versions other than 103** (Fallout 3 = 104, Skyrim SE = 105) are not handled
  by the dedicated Oblivion reader. They are detected as `FO3`/`SSE` and read through the
  generic TES4-family `Bsa::CompressedBSAFile`, without the Oblivion reader's validation
  and caching. The `Unknown100`/`Unknown200` bits ("embedded names" / XMEM codec) carry no
  structural meaning in Oblivion and are ignored.
* **No archive writing/modification** anywhere; all BSA support is read-only.
* **Embedded-name archives are not addressable by name.** Archives without a file name
  table are readable only by hash lookup; the VFS integration lists files by name, so such
  archives cannot be mounted by name (official Oblivion archives all ship name tables).

### Engine behaviour

* **Load order is enforced strictly, but only at startup for Oblivion content.** Missing
  or misordered masters are warnings by default; set `strict load order` (or
  `--oblivion-strict-load-order`) to make them fatal.
* **Oblivion distant terrain / large world LOD resources** (as generated by e.g. TES4LODGen)
  are not handled.
* **Oblivion water, weather, terrain, lighting and sky models differ from Morrowind's.**
  Where the engine needs Oblivion-specific behaviour that does not exist yet, Morrowind
  behaviour applies.
* **Multiplayer / Lua** are unaffected, but note that most Lua bindings reflect Morrowind
  semantics; they have not been extended for Oblivion.
* **bsatool** recognises the new `TES4`/`FO3`/`SSE` archive version values and handles
  Oblivion archives through the generic TES4-family reader; it does not use the dedicated
  Oblivion BSA reader. Archive *creation* or *modification* is not supported anywhere.
* **esmtool is TES4-family aware, not Oblivion specific.** It can inspect Oblivion files
  as generic TES4-family records but has no Oblivion validation pass. Verified against
  `Oblivion.esm`: `esmtool dump [-t <RECORD>] [-q] Oblivion.esm` completes cleanly; note
  that record-type filtering uses the TES4 four-character codes (`-t NPCE`, not `-t NPC`).
* **Headless launches on a machine without audio input/output or a window manager** work,
  but if the engine throws during startup while stdin is not a terminal it pops a modal
  SDL error dialog that blocks forever with nobody to dismiss it (the real error message is
  only logged *after* the dialog). When debugging headless runs, give the process a pty
  (e.g. `python3 -c "import pty; pty.spawn(['./build/openmw'])"`), or check for an
  unresponsive `openmw` child process holding the log.

### Content loading

* The Oblivion view produced by `Oblivion::ContentLoader` (cell/world extraction, record
  type statistics) is consumed by the content pipeline for validation and logging. It is
  not a replacement for the ESM4 store: records themselves are not duplicated into the
  engine's stores, and no engine system other than the startup check reads them yet.
* Loading a full Oblivion plugin is expensive (the whole file is parsed and its
  sub-records decoded). Use `validate()` (header only) when you do not need the records.
* The parser materialises all records and groups in memory. Very large plugins (e.g.
  heavily merged mod builds) will use accordingly much more memory than the streaming
  path.

## Reporting bugs

When reporting an Oblivion-related problem, include:

* the exact `openmw.cfg` (or the `--data`/`--content` command line),
* the `openmw --version` output,
* a verbose log (`--oblivion-verbose` / `verbose content loading = true`),
* whether the problem also occurs with `--oblivion-strict-load-order`,
* whether the problem reproduces with only `Oblivion.esm` configured.

File bugs on the issue tracker with the relevant label. Please do not attach copyrighted
game data; synthetic minimal plugins and BSAs (like those in the test suite) are perfect
for that.

## Related documentation

* [Advanced settings](source/reference/modding/settings/oblivion.rst) — the `[Oblivion]`
  settings page (Sphinx manual).
* [`docs/source/reference/modding/`](source/reference/modding/) — modding reference.
* [`files/settings-default.cfg`](../files/settings-default.cfg) — all default settings,
  including the `[Oblivion]` section.
* [`openmw-stage1.md`](openmw-stage1.md) — the general post-1.0 engine roadmap.
* [UESP Oblivion](https://en.uesp.net/wiki/Oblivion) — documentation of the game's
  mechanics, which the mechanics component encodes.
