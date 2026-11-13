#pragma once

#include <string>

namespace Kans
{

    enum class AssetErrorCode
    {
        InvalidArgument,
        NotFound,
        InvalidData,
        UnsupportedVersion,
        IoError,
        DuplicateID,
        DuplicatePath,
        NotImplemented,
        Cancelled,
        StaleShaderContract,
    };

    struct AssetError
    {
        AssetErrorCode Code;
        std::string    Message;
        AssetError(AssetErrorCode code, const std::string& message)
        {
            this->Code    = code;
            this->Message = message;
        }
    };

} // namespace Kans
