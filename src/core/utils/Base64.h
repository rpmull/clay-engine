#pragma once

#include <string>
#include <vector>

namespace cm::base64
{
    // Encode binary data into a base64 string (no newlines, padded with '=').
    std::string Encode(const std::vector<uint8_t>& input);
    // Decode a base64 string; returns false if the input is invalid.
    bool Decode(const std::string& input, std::vector<uint8_t>& outData);
}


