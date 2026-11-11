#ifndef OPENMW_COMPONENTS_SETTINGS_CATEGORIES_OBLIVION_H
#define OPENMW_COMPONENTS_SETTINGS_CATEGORIES_OBLIVION_H

#include <components/settings/settingvalue.hpp>

#include <cstdint>
#include <string>
#include <string_view>

namespace Settings
{
    struct OblivionCategory : WithIndex
    {
        using WithIndex::WithIndex;

        SettingValue<bool> mEnable{ mIndex, "Oblivion", "enable" };
        SettingValue<bool> mValidateContent{ mIndex, "Oblivion", "validate content" };
        SettingValue<bool> mStrictLoadOrder{ mIndex, "Oblivion", "strict load order" };
        SettingValue<bool> mVerboseContentLoading{ mIndex, "Oblivion", "verbose content loading" };
    };
}

#endif
