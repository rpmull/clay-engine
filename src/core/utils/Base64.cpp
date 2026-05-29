#include "core/utils/Base64.h"

#include <array>
#include <cctype>

namespace cm::base64
{
namespace
{
    constexpr const char kAlphabet[] =
        "ABCDEFGHIJKLMNOPQRSTUVWXYZ"
        "abcdefghijklmnopqrstuvwxyz"
        "0123456789+/";

    inline uint8_t EncodeValue(uint8_t v)
    {
        return static_cast<uint8_t>(kAlphabet[v & 0x3F]);
    }

    inline int DecodeValue(char c)
    {
        if (c >= 'A' && c <= 'Z') return c - 'A';
        if (c >= 'a' && c <= 'z') return c - 'a' + 26;
        if (c >= '0' && c <= '9') return c - '0' + 52;
        if (c == '+') return 62;
        if (c == '/') return 63;
        return -1;
    }
}

std::string Encode(const std::vector<uint8_t>& input)
{
    if (input.empty()) return {};

    std::string out;
    out.reserve(((input.size() + 2) / 3) * 4);

    size_t i = 0;
    while (i + 2 < input.size())
    {
        uint32_t block = (static_cast<uint32_t>(input[i]) << 16) |
                         (static_cast<uint32_t>(input[i + 1]) << 8) |
                         static_cast<uint32_t>(input[i + 2]);
        out.push_back(static_cast<char>(EncodeValue((block >> 18) & 0x3F)));
        out.push_back(static_cast<char>(EncodeValue((block >> 12) & 0x3F)));
        out.push_back(static_cast<char>(EncodeValue((block >> 6) & 0x3F)));
        out.push_back(static_cast<char>(EncodeValue(block & 0x3F)));
        i += 3;
    }

    if (i < input.size())
    {
        uint32_t block = static_cast<uint32_t>(input[i]) << 16;
        out.push_back(static_cast<char>(EncodeValue((block >> 18) & 0x3F)));
        if (i + 1 < input.size())
        {
            block |= static_cast<uint32_t>(input[i + 1]) << 8;
            out.push_back(static_cast<char>(EncodeValue((block >> 12) & 0x3F)));
            out.push_back(static_cast<char>(EncodeValue((block >> 6) & 0x3F)));
            out.push_back('=');
        }
        else
        {
            out.push_back(static_cast<char>(EncodeValue((block >> 12) & 0x3F)));
            out.push_back('=');
            out.push_back('=');
        }
    }

    return out;
}

bool Decode(const std::string& input, std::vector<uint8_t>& outData)
{
    outData.clear();
    if (input.empty()) return true;

    if (input.size() % 4 != 0) return false;

    size_t padding = 0;
    if (!input.empty())
    {
        if (input[input.size() - 1] == '=') padding++;
        if (input[input.size() - 2] == '=') padding++;
    }

    outData.reserve(((input.size() / 4) * 3) - padding);

    for (size_t i = 0; i < input.size(); i += 4)
    {
        int vals[4];
        for (int j = 0; j < 4; ++j)
        {
            char c = input[i + j];
            if (c == '=')
            {
                vals[j] = -2; // padding sentinel
            }
            else
            {
                vals[j] = DecodeValue(c);
                if (vals[j] < 0) return false;
            }
        }

        uint32_t block = 0;
        block |= (vals[0] & 0x3F) << 18;
        block |= (vals[1] & 0x3F) << 12;

        bool thirdValid = vals[2] >= 0;
        bool fourthValid = vals[3] >= 0;

        if (thirdValid) block |= (vals[2] & 0x3F) << 6;
        if (fourthValid) block |= (vals[3] & 0x3F);

        outData.push_back(static_cast<uint8_t>((block >> 16) & 0xFF));
        if (thirdValid)
        {
            outData.push_back(static_cast<uint8_t>((block >> 8) & 0xFF));
        }
        if (fourthValid)
        {
            outData.push_back(static_cast<uint8_t>(block & 0xFF));
        }
    }

    return true;
}

} // namespace cm::base64


