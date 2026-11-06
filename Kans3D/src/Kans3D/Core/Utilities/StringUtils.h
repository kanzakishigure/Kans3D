#pragma once
#include <cstdint>
#include <string>
#include <vector>
namespace Kans::Utils
{
    std::vector<std::string> SplitString(const std::string& token);

    bool        HexToUint64(const std::string& s, uint64_t& out);
    std::string Uint64ToHex(uint64_t value);
} // namespace Kans::Utils
