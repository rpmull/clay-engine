#pragma once
#include <cstdint>
#include <string>
#include <string_view>
#include <functional>
#include <iosfwd> // forward-declare std::ostream

namespace cm {

   // Portable 128-bit ID with stable, deterministic hashing from a name.
   struct TypeId {
      uint64_t hi = 0;
      uint64_t lo = 0;

      constexpr TypeId() = default;
      constexpr TypeId(uint64_t hi_, uint64_t lo_) : hi(hi_), lo(lo_) {}

      // C++17 comparisons (no <=>)
      friend constexpr bool operator==(const TypeId& a, const TypeId& b) noexcept {
         return a.hi == b.hi && a.lo == b.lo;
         }
      friend constexpr bool operator!=(const TypeId& a, const TypeId& b) noexcept {
         return !(a == b);
         }
      friend constexpr bool operator<(const TypeId& a, const TypeId& b) noexcept {
         return (a.hi < b.hi) || (a.hi == b.hi && a.lo < b.lo);
         }

      // Deterministic from fully-qualified name, e.g. "RPG.Stats"
      static TypeId FromName(std::string_view fullName) noexcept;

      // 32 lowercase hex chars (hi then lo)
      std::string ToHex() const;
      static bool TryParseHex(std::string_view hex32, TypeId& out) noexcept;

      // Combine two ids (useful for derived/templated concepts)
      static TypeId Combine(const TypeId& a, const TypeId& b) noexcept;
      };

   // Hash functor for unordered_map/set
   struct TypeIdHasher {
      size_t operator()(const TypeId& id) const noexcept;
      };

   // Stream helper (defined in .cpp)
   std::ostream& operator<<(std::ostream& os, const TypeId& id);

   } // namespace cm
