#pragma once

// Compile-time string utilities for building rfl::Description strings
// from constants. Follows the same patterns reflect-cpp uses internally
// (e.g., in remove_namespaces.hpp).

#include <array>
#include <cstddef>

#include <rfl/internal/StringLiteral.hpp>

namespace openmoq::moqx::config {

// Concatenate any number of StringLiterals into one.
template <rfl::internal::StringLiteral... Parts> consteval auto str_concat() {
  constexpr size_t N = (... + (Parts.string_view().size())) + 1;
  std::array<char, N> arr{};
  size_t offset = 0;
  auto append = [&](auto part) {
    for (auto c : part.string_view()) {
      arr[offset++] = c;
    }
  };
  (append(Parts), ...);
  arr[offset] = '\0';
  return rfl::internal::StringLiteral<N>(arr);
}

// Convert an unsigned integer to a StringLiteral at compile time.
template <unsigned long long V> consteval auto uint_to_str() {
  if constexpr (V == 0) {
    return rfl::internal::StringLiteral("0");
  } else {
    constexpr auto num_digits = [] {
      unsigned long long n = V;
      int c = 0;
      while (n > 0) {
        n /= 10;
        c++;
      }
      return c;
    }();
    std::array<char, num_digits + 1> arr{};
    unsigned long long n = V;
    for (int i = num_digits - 1; i >= 0; --i) {
      arr[i] = '0' + static_cast<char>(n % 10);
      n /= 10;
    }
    arr[num_digits] = '\0';
    return rfl::internal::StringLiteral<num_digits + 1>(arr);
  }
}

// Convert a bool to a StringLiteral at compile time.
template <bool V> consteval auto bool_to_str() {
  if constexpr (V) {
    return rfl::internal::StringLiteral("true");
  } else {
    return rfl::internal::StringLiteral("false");
  }
}

// Compile-time self-tests
static_assert(uint_to_str<0>().string_view() == "0");
static_assert(uint_to_str<42>().string_view() == "42");
static_assert(uint_to_str<9668>().string_view() == "9668");
static_assert(bool_to_str<true>().string_view() == "true");
static_assert(bool_to_str<false>().string_view() == "false");
static_assert(str_concat<"hello", " ", "world">().string_view() == "hello world");

} // namespace openmoq::moqx::config
