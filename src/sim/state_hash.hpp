#pragma once
// Stable 64-bit simulation state hash: FNV-1a 64 over an explicit
// little-endian encoding of each value, so the result does not depend on
// host byte order, struct padding or std::hash (which is unspecified).
//
// Encoding rules:
//   bool                 1 byte (0 or 1)
//   integers             sizeof(T) bytes, little-endian two's complement
//                        (size_t is 8 bytes on the LP64 target ABIs)
//   enums                as their underlying integer
//   float / double       canonical IEEE-754 bit pattern via std::bit_cast (-0.0 -> +0.0, NaN -> qNaN)
//   strings              8-byte length, then the bytes
//   vector/deque/array   8-byte element count, then each element
//   optional             presence byte, then the value if engaged
//   pair                 first, then second
//   other types          hashValue(StateHasher&, const T&) found by ADL
// Pointers are never hashed: there is deliberately no overload for them.
#include <array>
#include <bit>
#include <cmath>
#include <concepts>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <optional>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>
#include <vector>

namespace ss2 {

class StateHasher {
public:
    static constexpr std::uint64_t kOffsetBasis = 0xcbf29ce484222325ull;
    static constexpr std::uint64_t kPrime = 0x100000001b3ull;

    std::uint64_t value() const noexcept { return h_; }

    void byte(std::uint8_t b) noexcept { h_ = (h_ ^ b) * kPrime; }

    template<std::unsigned_integral U>
    void le(U v) noexcept {
        for (std::size_t i = 0; i < sizeof(U); ++i) byte(static_cast<std::uint8_t>(v >> (8u * i)));
    }

    void size(std::size_t n) noexcept { le(static_cast<std::uint64_t>(n)); }

    void add(bool v) noexcept { byte(v ? 1u : 0u); }
    // Pointers would otherwise convert silently to bool; their values are not state.
    template<class T>
    void add(const T*) = delete;
    void add(std::nullptr_t) = delete;

    template<std::integral T>
        requires(!std::same_as<T, bool>)
    void add(T v) noexcept {
        le(static_cast<std::make_unsigned_t<T>>(v));
    }

    template<class E>
        requires std::is_enum_v<E>
    void add(E v) noexcept {
        add(static_cast<std::underlying_type_t<E>>(v));
    }

    void add(float v) noexcept {
        if (std::isnan(v)) {
            le(std::uint32_t{0x7fc00000u});
            return;
        }
        if (v == 0.0f) v = 0.0f;
        le(std::bit_cast<std::uint32_t>(v));
    }
    void add(double v) noexcept {
        if (std::isnan(v)) {
            le(std::uint64_t{0x7ff8000000000000ull});
            return;
        }
        if (v == 0.0) v = 0.0;
        le(std::bit_cast<std::uint64_t>(v));
    }

    void add(std::string_view s) noexcept {
        size(s.size());
        for (const char c : s) byte(static_cast<std::uint8_t>(c));
    }
    void add(const std::string& s) noexcept { add(std::string_view(s)); }

    template<class A, class B>
    void add(const std::pair<A, B>& p) {
        add(p.first);
        add(p.second);
    }

    template<class T>
    void add(const std::optional<T>& o) {
        add(o.has_value());
        if (o) add(*o);
    }

    template<class T, std::size_t N>
    void add(const std::array<T, N>& a) {
        each(a, [this](const T& e) { add(e); });
    }

    template<class T, class A>
    void add(const std::vector<T, A>& v) {
        each(v, [this](const T& e) { add(e); });
    }

    template<class A>
    void add(const std::vector<bool, A>& v) {
        size(v.size());
        for (const bool b : v) add(b);
    }

    template<class T, class A>
    void add(const std::deque<T, A>& v) {
        each(v, [this](const T& e) { add(e); });
    }

    // User types provide hashValue(StateHasher&, const T&) in their namespace.
    template<class T>
        requires requires(StateHasher& h, const T& t) { hashValue(h, t); }
    void add(const T& t) {
        hashValue(*this, t);
    }

    template<class... Ts>
    void all(const Ts&... vs) {
        (add(vs), ...);
    }

    // A sized range whose elements are hashed by f (e.g. private nested structs).
    template<class R, class F>
    void each(const R& r, F&& f) {
        size(std::size(r));
        for (const auto& e : r) f(e);
    }

    template<class T, class F>
    void opt(const std::optional<T>& o, F&& f) {
        add(o.has_value());
        if (o) f(*o);
    }

private:
    std::uint64_t h_{kOffsetBasis};
};

} // namespace ss2
