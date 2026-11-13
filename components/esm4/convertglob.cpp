#include "convertglob.hpp"

#include <cstdint>

#include <components/esm/refid.hpp>
#include <components/esm3/loadglob.hpp>
#include <components/esm3/variant.hpp>

#include "loadglob.hpp"

namespace ESM4
{
    std::optional<ESM::Global> convertToESM3(const GlobalVariable& glob)
    {
        ESM::VarType type;
        switch (glob.mType)
        {
            case 'f':
                type = ESM::VT_Float;
                break;
            case 's':
                type = ESM::VT_Short;
                break;
            case 'l':
                type = ESM::VT_Long;
                break;
            default:
                return std::nullopt;
        }

        ESM::Global global;
        global.mRecordFlags = 0;
        global.mId = ESM::RefId::stringRefId(glob.mEditorId);

        global.mValue.setType(type);
        if (type == ESM::VT_Float)
            global.mValue.setFloat(glob.mValue);
        else
            global.mValue.setInteger(static_cast<std::int32_t>(glob.mValue));

        return global;
    }
}
