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

#include "config.hpp"

#include <algorithm>
#include <fstream>
#include <filesystem>
#include <optional>
#include <stdexcept>
#include <unordered_map>
#include <utility>
#
#include <components/debug/debuglog.hpp>
#include <components/esm/format.hpp>
#include <components/files/collections.hpp>
#include <components/files/multidircollection.hpp>
#include <components/files/openfile.hpp>
#include <components/files/oblivion/esm.hpp>
#include <components/misc/pathhelpers.hpp>
#include <components/misc/strings/algorithm.hpp>
#include <components/misc/strings/lower.hpp>
#include <components/oblivion/contentloader.hpp>
#include <components/settings/values.hpp>

#include <boost/program_options/options_description.hpp>
#include <boost/program_options/variables_map.hpp>

namespace
{
    namespace bpo = boost::program_options;

    // Returns the value of the named variable if it was set explicitly on the command line
    // or in openmw.cfg (i.e. not left at its declared default). Returns std::nullopt otherwise.
    std::optional<bool> explicitBool(const bpo::variables_map& variables, std::string_view name)
    {
        const auto it = variables.find(std::string(name));
        if (it == variables.end() || it->second.defaulted())
            return std::nullopt;
        return it->second.as<bool>();
    }

    bool isTes4FamilyExtension(std::string_view extension)
    {
        return Misc::StringUtils::ciEqual(extension, "esm") || Misc::StringUtils::ciEqual(extension, "esp");
    }

    // Compare lowercased file names; Oblivion master dependencies are compared case-insensitively,
    // like the rest of the engine's load order checks.
    std::string lower(std::string_view value)
    {
        return Misc::StringUtils::lowerCase(value);
    }

    Oblivion::ConfigIssue makeIssue(Oblivion::ConfigIssueSeverity severity, const std::string& message)
    {
        return Oblivion::ConfigIssue{ severity, message };
    }

    // Load order issues are warnings by default and errors in strict mode.
    Oblivion::ConfigIssueSeverity loadOrderSeverity(const Oblivion::Configuration& config)
    {
        return config.mStrictLoadOrder ? Oblivion::ConfigIssueSeverity::Error : Oblivion::ConfigIssueSeverity::Warning;
    }
}

namespace Oblivion
{
    bool ContentReport::hasErrors() const
    {
        return std::ranges::any_of(mIssues, [](const ConfigIssue& issue) {
            return issue.mSeverity == ConfigIssueSeverity::Error;
        });
    }

    bool ContentReport::hasWarnings() const
    {
        return std::ranges::any_of(mIssues, [](const ConfigIssue& issue) {
            return issue.mSeverity == ConfigIssueSeverity::Warning;
        });
    }

    void addOptions(bpo::options_description& description)
    {
        // NOTE: option names are string literals (static storage duration), the kOption* constants
        // in the header are used for lookups; the static asserts keep them in sync.
        static_assert(kOptionOblivion == "oblivion");
        static_assert(kOptionValidate == "oblivion-validate");
        static_assert(kOptionStrictLoadOrder == "oblivion-strict-load-order");
        static_assert(kOptionVerbose == "oblivion-verbose");

        auto addOption = description.add_options();
        addOption("oblivion",
            bpo::value<bool>()->implicit_value(true)->default_value(false),
            "force Oblivion (TES4) mode for the session: content files that are not Oblivion plugins/masters are "
            "rejected, and Oblivion content files are always validated at startup");
        addOption("oblivion-validate",
            bpo::value<bool>()->implicit_value(true)->default_value(true),
            "validate Oblivion content files at startup (TES4 signature, HEDR version, master availability and load order)");
        addOption("oblivion-strict-load-order",
            bpo::value<bool>()->implicit_value(true)->default_value(false),
            "treat missing or misordered Oblivion master files as fatal errors instead of warnings");
        addOption("oblivion-verbose",
            bpo::value<bool>()->implicit_value(true)->default_value(false),
            "log detailed information about the Oblivion content files that are loaded");
    }

    Configuration loadConfiguration(const bpo::variables_map& variables)
    {
        Configuration config;

        // settings.cfg [Oblivion] values act as the base values; an option set explicitly
        // on the command line or in openmw.cfg takes precedence over them.
        config.mEnable = Settings::oblivion().mEnable.get();
        config.mValidate = Settings::oblivion().mValidateContent.get();
        config.mStrictLoadOrder = Settings::oblivion().mStrictLoadOrder.get();
        config.mVerbose = Settings::oblivion().mVerboseContentLoading.get();

        if (const auto forced = explicitBool(variables, kOptionOblivion))
            config.mForced = *forced;
        if (const auto validate = explicitBool(variables, kOptionValidate))
            config.mValidate = *validate;
        if (const auto strict = explicitBool(variables, kOptionStrictLoadOrder))
            config.mStrictLoadOrder = *strict;
        if (const auto verbose = explicitBool(variables, kOptionVerbose))
            config.mVerbose = *verbose;

        // Forcing Oblivion mode implies that the content files are validated; silently
        // skipping validation would defeat the purpose of the option.
        if (config.mForced)
            config.mValidate = true;

        return config;
    }

    ContentReport checkContentFiles(const std::vector<std::string>& contentFiles,
        const Files::Collections& collections, const Configuration& config)
    {
        ContentReport report;

        // Index of each configured content file by lowercased file name, used for the
        // master availability/load order checks. Indices reflect the load order used by the engine.
        std::unordered_map<std::string, std::size_t> indexByName;
        indexByName.reserve(contentFiles.size());
        for (std::size_t i = 0; i < contentFiles.size(); ++i)
            indexByName.emplace(lower(contentFiles[i]), i);

        for (std::size_t index = 0; index < contentFiles.size(); ++index)
        {
            const std::string& file = contentFiles[index];
            const std::string_view extension = Misc::getFileExtension(file);
            if (!isTes4FamilyExtension(extension))
                continue; // not a TES-family plugin/master; .omwgame/.omwaddon/.omwscripts are fine

            const Files::MultiDirCollection& collection = collections.getCollection(extension);
            if (!collection.doesExist(file))
            {
                report.mIssues.push_back(makeIssue(ConfigIssueSeverity::Error,
                    "Content file '" + file + "' does not exist in any of the configured data directories"));
                continue;
            }
            const std::filesystem::path path = collection.getPath(file);

            // Detect the on-disk format; TES3-family files are Morrowind content.
            ESM::Format format = ESM::Format::Tes3;
            bool formatRecognised = true;
            try
            {
                auto stream = Files::openBinaryInputFileStream(path);
                format = ESM::readFormat(*stream);
            }
            catch (const std::exception&)
            {
                formatRecognised = false;
            }

            if (!formatRecognised || format == ESM::Format::Tes3)
            {
                const std::string message = !formatRecognised
                    ? "'" + file + "' is not a recognised TES3/TES4 plugin/master (unreadable or corrupt file)"
                    : "'" + file + "' is a Morrowind (TES3) plugin/master, not Oblivion content";
                if (config.mForced)
                    report.mIssues.push_back(makeIssue(ConfigIssueSeverity::Error,
                        message + "; Oblivion mode was forced"));
                else if (!formatRecognised)
                    report.mIssues.push_back(makeIssue(ConfigIssueSeverity::Warning, message));
                else
                    report.mIssues.push_back(makeIssue(ConfigIssueSeverity::Info, message));
                continue;
            }

            // From here on: TES4-family file.
            if (!config.mEnable && !config.mForced)
            {
                report.mIssues.push_back(makeIssue(ConfigIssueSeverity::Error,
                    "Oblivion content file '" + file + "' is configured, but Oblivion support is disabled "
                    "([Oblivion] enable = false in settings.cfg)"));
                continue;
            }

            const bool isOblivion = ContentLoader::isOblivionPlugin(path);
            if (!isOblivion)
            {
                const std::string message = "'" + file
                    + "' is a TES4-family plugin/master, but its HEDR version is not recognised as Oblivion";
                if (config.mForced)
                    report.mIssues.push_back(makeIssue(ConfigIssueSeverity::Error,
                        message + "; Oblivion mode was forced"));
                else
                    report.mIssues.push_back(makeIssue(ConfigIssueSeverity::Warning,
                        message + "; it will not be validated or checked for master dependencies"));
                continue;
            }
            ++report.mOblivionContentCount;

            if (!config.mValidate)
            {
                if (config.mVerbose)
                    report.mIssues.push_back(makeIssue(ConfigIssueSeverity::Info,
                        "Oblivion content file '" + file + "' (validation disabled)"));
                continue;
            }

            Files::Oblivion::Header header{};
            try
            {
                header = ContentLoader::validate(path);
            }
            catch (const std::exception& error)
            {
                report.mIssues.push_back(makeIssue(ConfigIssueSeverity::Error,
                    "Oblivion plugin '" + file + "' is invalid: " + error.what()));
                continue;
            }

            if (config.mVerbose)
            {
                std::string masters;
                for (const ESM::MasterData& master : header.mMaster)
                {
                    if (!masters.empty())
                        masters += ", ";
                    masters += master.name;
                }
                report.mIssues.push_back(makeIssue(ConfigIssueSeverity::Info,
                    "Oblivion plugin '" + file + "' (HEDR version "
                        + std::to_string(header.mData.version.f) + ", author '" + header.mAuthor
                        + "', masters: " + (masters.empty() ? std::string("none") : masters) + ")"));
            }

            // Check every master dependency against the configured content list and the data directories.
            for (const ESM::MasterData& master : header.mMaster)
            {
                const auto it = indexByName.find(lower(master.name));
                if (it == indexByName.end())
                {
                    std::string message = "Oblivion plugin '" + file + "' requires master file '" + master.name
                        + "', which is not listed in the content files";
                    if (collections.doesExist(master.name))
                        message += "; the file exists in a data directory, but must also be added to the content "
                                   "list before '"
                            + file + "'";
                    else
                        message += " and was not found in the data directories either";
                    report.mIssues.push_back(makeIssue(loadOrderSeverity(config), std::move(message)));
                }
                else if (it->second > index)
                {
                    report.mIssues.push_back(makeIssue(loadOrderSeverity(config),
                        "Oblivion master file '" + master.name + "' is listed after '" + file
                            + "' in the content files; masters must be loaded before the plugins that depend on them"));
                }
            }
        }

        return report;
    }

    void logContentReport(const ContentReport& report)
    {
        std::size_t info = 0;
        std::size_t warnings = 0;
        std::size_t errors = 0;
        for (const ConfigIssue& issue : report.mIssues)
        {
            switch (issue.mSeverity)
            {
                case ConfigIssueSeverity::Info:
                    ++info;
                    Log(Debug::Info) << "Oblivion configuration: " << issue.mMessage;
                    break;
                case ConfigIssueSeverity::Warning:
                    ++warnings;
                    Log(Debug::Warning) << "Oblivion configuration: " << issue.mMessage;
                    break;
                case ConfigIssueSeverity::Error:
                    ++errors;
                    Log(Debug::Error) << "Oblivion configuration: " << issue.mMessage;
                    break;
            }
        }
        Log(Debug::Info) << "Oblivion configuration: checked content files, " << report.mOblivionContentCount
                         << " Oblivion plugin(s)/master(s), " << info << " info, " << warnings << " warning(s), "
                         << errors << " error(s)";
    }

} /* namespace Oblivion */
