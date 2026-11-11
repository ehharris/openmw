#ifndef OPENMW_COMPONENTS_OBLIVION_CONFIG_H
#define OPENMW_COMPONENTS_OBLIVION_CONFIG_H

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
  Oblivion-specific engine configuration.

  This component wires Oblivion support into the engine's configuration
  system and into the launch process:

    - command line options (and their equivalents in openmw.cfg, which is
      parsed with the same option descriptions) are added to the engine's
      options description with addOptions(). The available options are:

        oblivion                      force Oblivion (TES4) mode for the session
        oblivion-validate             validate Oblivion content files at startup
        oblivion-strict-load-order    missing/misordered Oblivion masters are fatal
        oblivion-verbose              log detailed information about Oblivion content files

      bool options accept the usual boost::program_options syntax
      ('--oblivion', '--oblivion=false', 'oblivion=true' in openmw.cfg).

    - configuration settings are read from the [Oblivion] section of
      settings.cfg (see files/settings-default.cfg). They are used for
      options that are *not* set on the command line / in openmw.cfg:

        [Oblivion]
        enable = true                 master switch for Oblivion (TES4) content support
        validate content = true       see oblivion-validate
        strict load order = false     see oblivion-strict-load-order
        verbose content loading = false  see oblivion-verbose

      Settings.cfg values are the fallbacks; an option passed on the command
      line or in openmw.cfg takes precedence over the corresponding
      settings.cfg value.

    - content files configured for launch are checked against Oblivion's
      rules before the engine loads them (checkContentFiles()): TES4
      plugins/masters are detected, their headers are validated, and their
      master dependencies are checked for availability and load order. The
      results are reported as a ContentReport with issues of different
      severities; the caller decides whether to abort the launch.

    - loadConfiguration() resolves the effective Oblivion::Configuration
      from the parsed command line / openmw.cfg variables (as produced by
      Files::ConfigurationManager::readConfiguration) and from settings.cfg
      (as loaded by Settings::Manager::load). Call it after
      Settings::Manager::load() and before checkContentFiles().

  The functions here are intentionally free functions (no global state) and
  do not cache anything across calls; Configuration values are plain data
  that can be copied freely.
*/

#include <cstddef>
#include <string>
#include <string_view>
#include <vector>

// NOLINTBEGIN(readability-identifier-naming)
namespace boost::program_options
{
    class options_description;
    class variables_map;
}
// NOLINTEND(readability-identifier-naming)

namespace Files
{
    class Collections;
}

namespace Oblivion
{
    // Command line / openmw.cfg option names.
    inline constexpr std::string_view kOptionOblivion = "oblivion";
    inline constexpr std::string_view kOptionValidate = "oblivion-validate";
    inline constexpr std::string_view kOptionStrictLoadOrder = "oblivion-strict-load-order";
    inline constexpr std::string_view kOptionVerbose = "oblivion-verbose";

    // Oblivion-specific configuration, resolved from settings.cfg and the
    // command line / openmw.cfg.
    struct Configuration
    {
        bool mEnable = true; ///< Oblivion (TES4) content support is enabled
        bool mValidate = true; ///< validate Oblivion content files at startup
        bool mStrictLoadOrder = false; ///< missing/misordered masters are fatal instead of warnings
        bool mVerbose = false; ///< log detailed information about Oblivion content files
        bool mForced = false; ///< Oblivion mode was forced via the 'oblivion' option
    };

    // Add the Oblivion-specific command line options to the given options description.
    // Call from the engine's options setup (apps/openmw/options.cpp) so that the
    // options are also accepted in openmw.cfg.
    void addOptions(boost::program_options::options_description& description);

    // Resolve the effective Oblivion configuration. Precedence:
    // explicit command line / openmw.cfg option > settings.cfg [Oblivion] > built-in default.
    // Forcing Oblivion mode (kOptionOblivion) also implies that content validation is performed.
    // Call after Settings::Manager::load().
    Configuration loadConfiguration(const boost::program_options::variables_map& variables);

    // Severity of an issue found while checking the configured content files.
    enum class ConfigIssueSeverity
    {
        Info,
        Warning,
        Error
    };

    // A single issue reported while checking the configured content files.
    struct ConfigIssue
    {
        ConfigIssueSeverity mSeverity = ConfigIssueSeverity::Info;
        std::string mMessage;
    };

    // Result of validating the configured content files.
    struct ContentReport
    {
        // Number of content files that are Oblivion (TES4 family) plugins or masters.
        std::size_t mOblivionContentCount = 0;
        std::vector<ConfigIssue> mIssues;

        bool hasOblivionContent() const { return mOblivionContentCount != 0; }
        bool hasErrors() const;
        bool hasWarnings() const;
    };

    // Check the content files configured for launch (the files passed to the engine,
    // e.g. via 'content' in openmw.cfg). The check is read-only and never throws; problems
    // are collected in the returned report.
    //
    // For every TES4-family (.esm/.esp) content file:
    //   - the file is located through the collections (configured data directories);
    //     a missing file is an error,
    //   - its signature is checked (TES3 content files are reported as info, or as an
    //     error if Oblivion mode was forced),
    //   - Oblivion plugins/masters are validated (HEDR version) when validation is enabled,
    //   - every master dependency is checked against the configured content list (a master
    //     must be listed before the plugin that needs it) and against the data directories;
    //     missing or misordered masters are warnings, or errors with mStrictLoadOrder set.
    // Other content files (.omwgame/.omwaddon/.omwscripts, groundcover, ...) are ignored.
    // Content files are checked in the order given, which is the load order used by the engine.
    ContentReport checkContentFiles(const std::vector<std::string>& contentFiles,
        const Files::Collections& collections, const Configuration& config);

    // Log the contents of the report (one line per issue and a summary line).
    // Issues of severity Error are logged as errors, Warning as warnings, Info as info.
    void logContentReport(const ContentReport& report);

} /* namespace Oblivion */

#endif /* OPENMW_COMPONENTS_OBLIVION_CONFIG_H */
