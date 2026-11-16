#pragma once
#include <cmath>
#include <sstream>
#include <string_view>
#include <variant>
#include <vector>
#include <glm/glm.hpp>
#include "Kans3D/Asset/AssetError.h"

namespace Kans
{
    struct CubeLutSource
    {
        uint32_t  Size = 0;
        glm::vec3 DomainMin {0}, DomainMax {1};
        // Adobe/Resolve .cube order: red changes fastest, then green, then blue.
        std::vector<glm::vec3> Values;
    };
    inline std::variant<CubeLutSource, AssetError> ParseCubeLutSource(std::string_view text)
    {
        CubeLutSource      result;
        bool               sizeSeen = false, minSeen = false, maxSeen = false, titleSeen = false, dataSeen = false;
        auto               error = [] { return AssetError {AssetErrorCode::InvalidData, "Invalid 3D .cube LUT"}; };
        std::istringstream input {std::string(text)};
        std::string        line;
        while (std::getline(input, line))
        {
            // A quoted title can contain '#'; comments outside titles begin at '#'.
            bool quoted = false;
            for (size_t i = 0; i < line.size(); ++i)
            {
                if (line[i] == '"')
                    quoted = !quoted;
                if (line[i] == '#' && !quoted)
                {
                    line.resize(i);
                    break;
                }
            }
            std::istringstream row(line);
            std::string        key;
            if (!(row >> key))
                continue;
            if (key == "TITLE")
            {
                if (titleSeen || dataSeen)
                    return error();
                titleSeen = true;
                std::string title;
                std::getline(row >> std::ws, title);
                const auto end = title.find_last_not_of(" \t\r");
                if (end == std::string::npos || title.front() != '"' || end == 0 || title[end] != '"' ||
                    title.find('"', 1) != end)
                    return error();
                continue;
            }
            if (key == "LUT_3D_SIZE")
            {
                if (sizeSeen || dataSeen || !(row >> result.Size) || result.Size < 17 || result.Size > 65)
                    return error();
                sizeSeen = true;
                result.Values.reserve(size_t(result.Size) * result.Size * result.Size);
            }
            else if (key == "DOMAIN_MIN" || key == "DOMAIN_MAX")
            {
                auto& seen  = key == "DOMAIN_MIN" ? minSeen : maxSeen;
                auto& value = key == "DOMAIN_MIN" ? result.DomainMin : result.DomainMax;
                if (seen || dataSeen || !(row >> value.x >> value.y >> value.z))
                    return error();
                seen = true;
                for (int ch = 0; ch < 3; ++ch)
                    if (!std::isfinite(value[ch]))
                        return error();
            }
            else
            {
                if (!sizeSeen)
                    return error();
                dataSeen = true;
                glm::vec3          value;
                std::istringstream number(key);
                char               extra;
                if (!(number >> value.x) || (number >> extra) || !(row >> value.y >> value.z))
                    return error();
                for (int ch = 0; ch < 3; ++ch)
                    if (!std::isfinite(value[ch]))
                        return error();
                if (result.Values.size() >= size_t(result.Size) * result.Size * result.Size)
                    return error();
                result.Values.push_back(value);
            }
            std::string extra;
            if (row >> extra)
                return error();
        }
        if (!sizeSeen || result.Values.size() != size_t(result.Size) * result.Size * result.Size)
            return error();
        for (int ch = 0; ch < 3; ++ch)
            if (result.DomainMax[ch] <= result.DomainMin[ch])
                return error();
        return result;
    }
} // namespace Kans
