#include "TypeId.h"
#include <array>
#include <ostream>

namespace {

   // splitmix64 mixer
   static inline uint64_t splitmix64(uint64_t x) noexcept {
      x += 0x9E3779B97F4A7C15ull;
      x = (x ^ (x >> 30)) * 0xBF58476D1CE4E5B9ull;
      x = (x ^ (x >> 27)) * 0x94D049BB133111EBull;
      return x ^ (x >> 31);
      }

   // FNV-1a 64
   static inline uint64_t fnv1a64(const unsigned char* data, size_t len, uint64_t seed = 14695981039346656037ull) noexcept {
      uint64_t h = seed;
      constexpr uint64_t P = 1099511628211ull;
      for (size_t i = 0; i < len; ++i) {
         h ^= static_cast<uint64_t>(data[i]);
         h *= P;
         }
      return h;
      }

   // Build 128-bit hash from name (deterministic, non-crypto)
   static inline std::pair<uint64_t, uint64_t> hash128_from_string(std::string_view s) noexcept {
      const auto* bytes = reinterpret_cast<const unsigned char*>(s.data());
      const size_t len = s.size();

      uint64_t h1 = fnv1a64(bytes, len);

      uint64_t h2_seed = 1099511628211ull ^ (h1 * 0x9E3779B185EBCA87ull);
      uint64_t h2 = h2_seed;
      for (size_t i = 0; i < len; ++i) {
         unsigned char b = bytes[len - 1 - i];
         h2 ^= static_cast<uint64_t>(b);
         h2 *= 1099511628211ull;
         }
      h2 = splitmix64(h2 ^ (h1 << 1));

      h1 = splitmix64(h1 + (h2 << 32));
      h2 = splitmix64(h2 + (h1 << 24));
      return { h1, h2 };
      }

   static inline int hexval(char c) noexcept {
      if (c >= '0' && c <= '9') return c - '0';
      if (c >= 'a' && c <= 'f') return c - 'a' + 10;
      if (c >= 'A' && c <= 'F') return c - 'A' + 10;
      return -1;
      }

   } // anon

namespace cm {

   TypeId TypeId::FromName(std::string_view fullName) noexcept {
      auto p = hash128_from_string(fullName);
      return TypeId{ p.first, p.second };
      }

   std::string TypeId::ToHex() const {
      static const char* HEX = "0123456789abcdef";
      auto emit64 = [&](uint64_t v) {
         std::array<char, 32> buf{}; // 16 bytes -> 32 hex chars
         for (int i = 15; i >= 0; --i) {
            const unsigned byte = static_cast<unsigned>(v & 0xFFu);
            buf[2 * i + 0] = HEX[(byte >> 4) & 0xFu];
            buf[2 * i + 1] = HEX[(byte) & 0xFu];
            v >>= 8;
            }
         return std::string(buf.data(), buf.size());
         };
      return emit64(hi) + emit64(lo);
      }

   bool TypeId::TryParseHex(std::string_view hex32, TypeId& out) noexcept {
      if (hex32.size() != 32) return false;
      auto parse64 = [&](size_t off) -> std::pair<bool, uint64_t> {
         uint64_t v = 0;
         for (size_t i = 0; i < 16; ++i) {
            int h = hexval(hex32[off + i]);
            if (h < 0) return { false, 0 };
            v = (v << 4) | static_cast<uint64_t>(h);
            }
         return { true, v };
         };
      auto p1 = parse64(0);  if (!p1.first) return false;
      auto p2 = parse64(16); if (!p2.first) return false;
      out.hi = p1.second; out.lo = p2.second;
      return true;
      }

   TypeId TypeId::Combine(const TypeId& a, const TypeId& b) noexcept {
      uint64_t hi = splitmix64(a.hi ^ (b.hi + 0x9E3779B97F4A7C15ull));
      uint64_t lo = splitmix64(a.lo ^ (b.lo + 0xBF58476D1CE4E5B9ull));
      return TypeId{ hi, lo };
      }

   size_t TypeIdHasher::operator()(const TypeId& id) const noexcept {
      uint64_t x = id.hi ^ (id.lo * 0x9E3779B185EBCA87ull);
      // avalanche
      x ^= x >> 33; x *= 0xff51afd7ed558ccdULL;
      x ^= x >> 33; x *= 0xc4ceb9fe1a85ec53ULL;
      x ^= x >> 33;
      return static_cast<size_t>(x);
      }

   std::ostream& operator<<(std::ostream& os, const TypeId& id) {
      return os << id.ToHex();
      }

   } // namespace cm
