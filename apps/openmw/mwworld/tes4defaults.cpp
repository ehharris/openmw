#include "tes4defaults.hpp"

#include <string_view>

#include <components/esm/refid.hpp>
#include <components/esm3/loadgmst.hpp>
#include <components/esm3/variant.hpp>

#include "esmstore.hpp"

namespace MWWorld
{
    namespace
    {
        struct DefaultGameSetting
        {
            std::string_view mId;
            float mValue;
        };

        /// Mandatory ESM3 GMST records that engine code dereferences unconditionally. TES4 content files do not
        /// provide these, so a TES4-only session would crash at startup without them. Grow this table as new
        /// startup failures are found.
        constexpr DefaultGameSetting sDefaultGameSettings[] = {
            { "fSwimHeightScale", 1.35f },
        };
    }

    void seedTES4DefaultGameSettings(ESMStore& store)
    {
        Store<ESM::GameSetting>& gmsts = store.getWritable<ESM::GameSetting>();

        for (const auto& defaultSetting : sDefaultGameSettings)
        {
            const ESM::RefId id = ESM::RefId::stringRefId(defaultSetting.mId);
            if (gmsts.search(id) != nullptr)
                continue;

            ESM::GameSetting setting;
            setting.blank();
            setting.mId = id;
            setting.mValue = ESM::Variant(defaultSetting.mValue);
            store.insertStatic(setting);
        }
    }
}
