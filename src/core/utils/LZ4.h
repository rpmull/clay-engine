// LZ4 Fast Compression - Minimal implementation for Claymore Engine
// Based on the LZ4 algorithm by Yann Collet (BSD-2-Clause license)
// This is a simplified implementation optimized for speed over compression ratio
#pragma once

#include <cstdint>
#include <cstring>
#include <vector>
#include <algorithm>

namespace lz4 {

// LZ4 block format constants
constexpr int MIN_MATCH = 4;
constexpr int HASH_LOG = 12;
constexpr int HASH_SIZE = 1 << HASH_LOG;
constexpr int COPY_LENGTH = 8;
constexpr int ML_BITS = 4;
constexpr int ML_MASK = (1 << ML_BITS) - 1;
constexpr int RUN_BITS = 8 - ML_BITS;
constexpr int RUN_MASK = (1 << RUN_BITS) - 1;

// Returns max compressed size for given input size
inline size_t CompressBound(size_t inputSize) {
    return inputSize + (inputSize / 255) + 16;
}

// Hash function for match finding
inline uint32_t Hash(uint32_t sequence) {
    return (sequence * 2654435761U) >> (32 - HASH_LOG);
}

inline uint32_t Read32(const uint8_t* p) {
    uint32_t v;
    memcpy(&v, p, 4);
    return v;
}

inline void Write16(uint8_t* p, uint16_t v) {
    memcpy(p, &v, 2);
}

// Count matching bytes (up to limit)
inline size_t CountMatch(const uint8_t* a, const uint8_t* b, const uint8_t* limit) {
    const uint8_t* start = a;
    while (a < limit - 7) {
        uint64_t diff;
        memcpy(&diff, a, 8);
        uint64_t bval;
        memcpy(&bval, b, 8);
        diff ^= bval;
        if (diff) {
#ifdef _MSC_VER
            unsigned long idx;
            _BitScanForward64(&idx, diff);
            return (a - start) + (idx >> 3);
#else
            return (a - start) + (__builtin_ctzll(diff) >> 3);
#endif
        }
        a += 8; b += 8;
    }
    while (a < limit && *a == *b) { a++; b++; }
    return a - start;
}

// Compress data using LZ4 algorithm
// Returns compressed size, or 0 on failure
inline size_t Compress(const uint8_t* src, size_t srcSize, uint8_t* dst, size_t dstCapacity) {
    if (srcSize == 0) return 0;
    if (srcSize > 0x7E000000) return 0; // Too large
    
    uint32_t hashTable[HASH_SIZE] = {0};
    
    const uint8_t* ip = src;
    const uint8_t* anchor = src;
    const uint8_t* iend = src + srcSize;
    const uint8_t* mflimit = iend - 12; // Leave room for last literals
    const uint8_t* matchlimit = iend - 5;
    
    uint8_t* op = dst;
    uint8_t* oend = dst + dstCapacity;
    
    if (srcSize < 13) goto _last_literals;
    
    // First byte is always literal
    hashTable[Hash(Read32(ip))] = (uint32_t)(ip - src);
    ip++;
    
    // Main loop
    while (ip < mflimit) {
        // Find match
        uint32_t h = Hash(Read32(ip));
        uint32_t matchIdx = hashTable[h];
        hashTable[h] = (uint32_t)(ip - src);
        
        const uint8_t* match = src + matchIdx;
        
        // Check match validity
        if (matchIdx > 0 && ip - match <= 65535 && Read32(match) == Read32(ip)) {
            // Found match
            size_t litLen = ip - anchor;
            
            // Encode token
            uint8_t* token = op++;
            if (op >= oend) return 0;
            
            // Encode literal length
            if (litLen >= RUN_MASK) {
                *token = RUN_MASK << ML_BITS;
                size_t len = litLen - RUN_MASK;
                while (len >= 255) { if (op >= oend) return 0; *op++ = 255; len -= 255; }
                if (op >= oend) return 0;
                *op++ = (uint8_t)len;
            } else {
                *token = (uint8_t)(litLen << ML_BITS);
            }
            
            // Copy literals
            if (op + litLen > oend) return 0;
            memcpy(op, anchor, litLen);
            op += litLen;
            
            // Encode offset
            if (op + 2 > oend) return 0;
            Write16(op, (uint16_t)(ip - match));
            op += 2;
            
            // Count match length
            ip += MIN_MATCH;
            match += MIN_MATCH;
            size_t matchLen = CountMatch(ip, match, matchlimit);
            ip += matchLen;
            matchLen += MIN_MATCH - 4; // Subtract minimum already implied
            
            // Encode match length
            if (matchLen >= ML_MASK) {
                *token |= ML_MASK;
                matchLen -= ML_MASK;
                while (matchLen >= 255) { if (op >= oend) return 0; *op++ = 255; matchLen -= 255; }
                if (op >= oend) return 0;
                *op++ = (uint8_t)matchLen;
            } else {
                *token |= (uint8_t)matchLen;
            }
            
            anchor = ip;
            
            // Update hash
            if (ip < mflimit) hashTable[Hash(Read32(ip))] = (uint32_t)(ip - src);
        } else {
            ip++;
        }
    }
    
_last_literals:
    // Encode last literals
    {
        size_t lastLitLen = iend - anchor;
        if (op + 1 + lastLitLen + (lastLitLen / 255) > oend) return 0;
        
        if (lastLitLen >= RUN_MASK) {
            *op++ = RUN_MASK << ML_BITS;
            size_t len = lastLitLen - RUN_MASK;
            while (len >= 255) { *op++ = 255; len -= 255; }
            *op++ = (uint8_t)len;
        } else {
            *op++ = (uint8_t)(lastLitLen << ML_BITS);
        }
        memcpy(op, anchor, lastLitLen);
        op += lastLitLen;
    }
    
    return op - dst;
}

// Decompress LZ4 data
// Returns decompressed size, or 0 on failure
inline size_t Decompress(const uint8_t* src, size_t srcSize, uint8_t* dst, size_t dstCapacity) {
    if (srcSize == 0) return 0;
    
    const uint8_t* ip = src;
    const uint8_t* iend = src + srcSize;
    uint8_t* op = dst;
    uint8_t* oend = dst + dstCapacity;
    
    while (ip < iend) {
        // Get token
        uint8_t token = *ip++;
        
        // Literal length
        size_t litLen = token >> ML_BITS;
        if (litLen == RUN_MASK) {
            while (ip < iend) {
                uint8_t s = *ip++;
                litLen += s;
                if (s != 255) break;
            }
        }
        
        // Copy literals
        if (op + litLen > oend || ip + litLen > iend) return 0;
        memcpy(op, ip, litLen);
        op += litLen;
        ip += litLen;
        
        if (ip >= iend) break; // End of block
        
        // Get match offset
        if (ip + 2 > iend) return 0;
        uint16_t offset;
        memcpy(&offset, ip, 2);
        ip += 2;
        if (offset == 0 || op - dst < offset) return 0;
        
        const uint8_t* match = op - offset;
        
        // Match length
        size_t matchLen = (token & ML_MASK) + MIN_MATCH;
        if ((token & ML_MASK) == ML_MASK) {
            while (ip < iend) {
                uint8_t s = *ip++;
                matchLen += s;
                if (s != 255) break;
            }
        }
        
        // Copy match (handle overlapping)
        if (op + matchLen > oend) return 0;
        if (offset < 8) {
            // Slow path for short offsets
            for (size_t i = 0; i < matchLen; i++) {
                op[i] = match[i];
            }
            op += matchLen;
        } else {
            // Fast path
            uint8_t* copyEnd = op + matchLen;
            do {
                memcpy(op, match, 8);
                match += 8;
                op += 8;
            } while (op < copyEnd);
            op = copyEnd;
        }
    }
    
    return op - dst;
}

// Convenience wrappers using std::vector
inline std::vector<uint8_t> CompressVec(const std::vector<uint8_t>& input) {
    if (input.empty()) return {};
    std::vector<uint8_t> output(CompressBound(input.size()));
    size_t compressedSize = Compress(input.data(), input.size(), output.data(), output.size());
    if (compressedSize == 0) return {};
    output.resize(compressedSize);
    return output;
}

inline std::vector<uint8_t> DecompressVec(const std::vector<uint8_t>& input, size_t uncompressedSize) {
    if (input.empty()) return {};
    std::vector<uint8_t> output(uncompressedSize);
    size_t decompressedSize = Decompress(input.data(), input.size(), output.data(), output.size());
    if (decompressedSize != uncompressedSize) return {};
    return output;
}

} // namespace lz4

