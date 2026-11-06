#include "kspch.h"
#include "StringUtils.h"
#include <cstdint>

namespace Kans::Utils
{

    std::vector<std::string> SplitString(const std::string& token)
    {
        std::vector<std::string> tokens;
        std::string              substring = token;
        size_t                   fpos      = substring.find_first_of("#");
        tokens.emplace_back()              = substring.substr(0, fpos + 1);
        substring                          = substring.substr(fpos + 1, substring.length());
        fpos                               = substring.find_first_of(" ");

        while (fpos != std::string::npos)
        {
            tokens.emplace_back() = substring.substr(0, fpos);
            substring             = substring.substr(fpos + 1, substring.length());
            fpos                  = substring.find_first_of(" ");
        }
        tokens.emplace_back() = substring;
        return tokens;
    }
    bool HexToUint64(const std::string& s, uint64_t& out)
    {
        uint64_t v = 0;
        for (char c : s)
        {
            int d;
            if (c >= '0' && c <= '9')
                d = c - '0';
            else if (c >= 'a' && c <= 'f')
                d = c - 'a' + 10;
            else if (c >= 'A' && c <= 'F')
                d = c - 'A' + 10;
            else
                return false; // 非法字符

            // 检查溢出：v * 16 + d 是否超过 UINT64_MAX
            if (v > (UINT64_MAX >> 4))
                return false;
            v = (v << 4) | d;
        }
        out = v;
        return true;
    }
    std::string Uint64ToHex(uint64_t value)
    {
        static const char* digits = "0123456789abcdef";
        std::string        s(16, '0');
        for (int i = 15; i >= 0; --i)
        {
            s[i] = digits[value & 0xF];
            value >>= 4;
        }
        return s;
    }
} // namespace Kans::Utils
