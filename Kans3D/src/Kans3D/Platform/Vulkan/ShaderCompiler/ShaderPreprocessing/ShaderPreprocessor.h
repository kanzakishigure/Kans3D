#pragma once
#include "Kans3D/Platform/Vulkan/VulkanShaderUtils.h"
#include "Kans3D/Utilities/StringUtils.h"

#include <vulkan/vulkan.h>

#include <filesystem>
#include <sstream>
#include <stdexcept>
#include <unordered_set>
namespace Kans
{
    namespace PreprocessUtils
    {
        // From https://wandbox.org/permlink/iXC7DWaU8Tk8jrf3 and is modified.
        enum class State : char
        {
            SlashOC,
            StarIC,
            SingleLineComment,
            MultiLineComment,
            NotAComment
        };

        template<typename InputIt, typename OutputIt>
        void CopyWithoutComments(InputIt first, InputIt last, OutputIt out)
        {
            State state = State::NotAComment;

            while (first != last)
            {
                switch (state)
                {
                    case State::SlashOC:
                        if (*first == '/')
                            state = State::SingleLineComment;
                        else if (*first == '*')
                            state = State::MultiLineComment;
                        else
                        {
                            state  = State::NotAComment;
                            *out++ = '/';
                            *out++ = *first;
                        }
                        break;
                    case State::StarIC:
                        if (*first == '/')
                            state = State::NotAComment;
                        else
                            state = State::MultiLineComment;
                        break;
                    case State::NotAComment:
                        if (*first == '/')
                            state = State::SlashOC;
                        else
                            *out++ = *first;
                        break;
                    case State::SingleLineComment:
                        if (*first == '\n')
                        {
                            state  = State::NotAComment;
                            *out++ = '\n';
                        }
                        break;
                    case State::MultiLineComment:
                        if (*first == '*')
                            state = State::StarIC;
                        else if (*first == '\n')
                            *out++ = '\n';
                        break;
                }
                ++first;
            }
        }

        template<bool RemoveHeaderGuard = false>
        bool ContainsHeaderGuard(std::string& headerSource)
        {
            size_t pos = headerSource.find('#');
            while (pos != std::string::npos)
            {
                const size_t endOfLine = headerSource.find_first_of("\r\n", pos);
                auto         tokens    = Utils::SplitString(headerSource.substr(pos, endOfLine - pos));
                auto         it        = tokens.begin();
                it++; // skip #
                if (*it == "pragma")
                {
                    if (*(++it) == "once")
                    {
                        if constexpr (RemoveHeaderGuard)
                        {
                            headerSource.erase(pos, endOfLine - pos);
                            return true;
                        }
                    }
                }
                pos = headerSource.find('#', pos + 1);
            }
            return false;
        }

    }; // namespace PreprocessUtils

    struct IncludeHeadData
    {
        std::filesystem::path IncludedFilePath {};
        size_t                IncludeDepth {};
        bool                  IsRelative {false};
        std::string           HashValue {};

        inline bool operator==(const IncludeHeadData& other) const
        {
            return this->IncludedFilePath == other.IncludedFilePath && this->HashValue == other.HashValue;
        }
    };

    struct HeaderSourceCache
    {
        std::string Source;
        std::string SourceHash;
        bool        IsGuarded;
    };
} // namespace Kans

namespace std
{
    template<>
    struct hash<Kans::IncludeHeadData>
    {
        size_t operator()(const Kans::IncludeHeadData& data) const
        {
            return ((*(uint32_t*)data.HashValue.data()) ^ std::filesystem::hash_value(data.IncludedFilePath));
        }
    };

} // namespace std

namespace Kans
{
    class ShaderPreprocessor
    {

    public:
        template<typename ShaderUtils::ShaderLang Lang>
        static std::map<VkShaderStageFlagBits, std::string> PreprocessShader(const std::string& source);

        template<typename ShaderUtils::ShaderLang Lang>
        static void PreprocessHeader(std::string& contents, bool& isGuarded, const std::filesystem::path& fullPath);
    };

    template<typename ShaderUtils::ShaderLang Lang>
    std::map<VkShaderStageFlagBits, std::string> ShaderPreprocessor::PreprocessShader(const std::string& source)
    {
        std::stringstream filtered;
        PreprocessUtils::CopyWithoutComments(source.begin(), source.end(), std::ostream_iterator<char>(filtered));
        std::istringstream                           lines(filtered.str());
        std::map<VkShaderStageFlagBits, std::string> stages;
        VkShaderStageFlagBits                        current = VK_SHADER_STAGE_FLAG_BITS_MAX_ENUM;
        std::string                                  text, line;
        auto                                         flush = [&] {
            if (current == VK_SHADER_STAGE_FLAG_BITS_MAX_ENUM)
                return;
            if (!stages.emplace(current, text).second)
                throw std::invalid_argument("Duplicate shader stage");
            text.clear();
            current = VK_SHADER_STAGE_FLAG_BITS_MAX_ENUM;
        };
        while (std::getline(lines, line))
        {
            const auto first     = line.find_first_not_of(" \t\r");
            const auto directive = first == std::string::npos ? std::string() : line.substr(first);
            const bool typed     = directive.rfind("#type", 0) == 0;
            const auto pragma    = directive.find("stage");
            const bool staged    = directive.rfind("#pragma", 0) == 0 && pragma != std::string::npos;
            if (typed || staged)
            {
                if (typed || current != VK_SHADER_STAGE_FLAG_BITS_MAX_ENUM)
                    flush();
                auto       name  = directive.substr(typed ? 5 : pragma + 5);
                const auto begin = name.find_first_not_of(" :\t");
                name             = begin == std::string::npos ? "" : name.substr(begin);
                name             = name.substr(0, name.find_first_of(" \t\r"));
                current          = name == "compute" || name == "comp"  ? VK_SHADER_STAGE_COMPUTE_BIT :
                                   name == "vertex" || name == "vert"   ? VK_SHADER_STAGE_VERTEX_BIT :
                                   name == "fragment" || name == "frag" ? VK_SHADER_STAGE_FRAGMENT_BIT :
                                   name == "geometry" || name == "geo"  ? VK_SHADER_STAGE_GEOMETRY_BIT :
                                                                          VK_SHADER_STAGE_FLAG_BITS_MAX_ENUM;
                if (current == VK_SHADER_STAGE_FLAG_BITS_MAX_ENUM)
                    throw std::invalid_argument("Unknown shader stage: " + name);
                continue;
            }
            if (directive.rfind("#version", 0) == 0 && text.find("#version") != std::string::npos)
                flush();
            text += line + "\n";
        }
        flush();
        if (stages.empty())
            throw std::invalid_argument("Shader source has no stage declaration");
        if (stages.count(VK_SHADER_STAGE_COMPUTE_BIT) && stages.size() != 1)
            throw std::invalid_argument("Compute shader cannot mix with graphics stages");
        return stages;
    }

    template<>
    inline void
    ShaderPreprocessor::PreprocessHeader<ShaderUtils::ShaderLang::GLSL>(std::string&                 contents,
                                                                        bool&                        isGuarded,
                                                                        const std::filesystem::path& fullPath)
    {
        std::stringstream sourceStream;
        PreprocessUtils::CopyWithoutComments(
            contents.begin(), contents.end(), std::ostream_iterator<char>(sourceStream));
        contents = sourceStream.str();

        isGuarded = PreprocessUtils::ContainsHeaderGuard<true>(contents);
    }

    template<>
    inline void
    ShaderPreprocessor::PreprocessHeader<ShaderUtils::ShaderLang::HLSL>(std::string&                 contents,
                                                                        bool&                        isGuarded,
                                                                        const std::filesystem::path& fullPath)
    {
        std::stringstream sourceStream;
        PreprocessUtils::CopyWithoutComments(
            contents.begin(), contents.end(), std::ostream_iterator<char>(sourceStream));
        contents = sourceStream.str();

        isGuarded = PreprocessUtils::ContainsHeaderGuard<false>(contents);
    }

} // namespace Kans