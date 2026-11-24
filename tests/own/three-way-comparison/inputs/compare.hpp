// The comparison category types as [cmp.categories] specifies them, for tests of the language's
// three-way comparison that do not depend on the rest of the library: a stand-in for <compare>.
#pragma once

namespace std {

class partial_ordering {
    signed char v_;
    constexpr explicit partial_ordering(signed char v) noexcept : v_(v) {}

public:
    static const partial_ordering less, equivalent, greater, unordered;
    friend constexpr bool operator==(partial_ordering a, partial_ordering b) noexcept { return a.v_ == b.v_; }
    friend constexpr bool operator==(partial_ordering a, int) noexcept { return a.v_ == 0; }
    friend constexpr bool operator<(partial_ordering a, int) noexcept { return a.v_ == -1; }
    friend constexpr bool operator>(partial_ordering a, int) noexcept { return a.v_ == 1; }
    friend constexpr bool operator<=(partial_ordering a, int) noexcept { return a.v_ == -1 || a.v_ == 0; }
    friend constexpr bool operator>=(partial_ordering a, int) noexcept { return a.v_ == 1 || a.v_ == 0; }
    friend constexpr bool operator<(int, partial_ordering a) noexcept { return a.v_ == 1; }
    friend constexpr bool operator>(int, partial_ordering a) noexcept { return a.v_ == -1; }
    friend constexpr bool operator<=(int, partial_ordering a) noexcept { return a.v_ == 1 || a.v_ == 0; }
    friend constexpr bool operator>=(int, partial_ordering a) noexcept { return a.v_ == -1 || a.v_ == 0; }
    friend constexpr partial_ordering operator<=>(partial_ordering a, int) noexcept { return a; }
    friend constexpr partial_ordering operator<=>(int, partial_ordering a) noexcept {
        if (a.v_ == 2) return a;
        return partial_ordering(static_cast<signed char>(-a.v_));
    }
};
inline constexpr partial_ordering partial_ordering::less(-1);
inline constexpr partial_ordering partial_ordering::equivalent(0);
inline constexpr partial_ordering partial_ordering::greater(1);
inline constexpr partial_ordering partial_ordering::unordered(2);

class weak_ordering {
    signed char v_;
    constexpr explicit weak_ordering(signed char v) noexcept : v_(v) {}

public:
    static const weak_ordering less, equivalent, greater;
    constexpr operator partial_ordering() const noexcept {
        if (v_ < 0) return partial_ordering::less;
        if (v_ > 0) return partial_ordering::greater;
        return partial_ordering::equivalent;
    }
    friend constexpr bool operator==(weak_ordering a, weak_ordering b) noexcept { return a.v_ == b.v_; }
    friend constexpr bool operator==(weak_ordering a, int) noexcept { return a.v_ == 0; }
    friend constexpr bool operator<(weak_ordering a, int) noexcept { return a.v_ < 0; }
    friend constexpr bool operator>(weak_ordering a, int) noexcept { return a.v_ > 0; }
    friend constexpr bool operator<=(weak_ordering a, int) noexcept { return a.v_ <= 0; }
    friend constexpr bool operator>=(weak_ordering a, int) noexcept { return a.v_ >= 0; }
    friend constexpr bool operator<(int, weak_ordering a) noexcept { return 0 < a.v_; }
    friend constexpr bool operator>(int, weak_ordering a) noexcept { return 0 > a.v_; }
    friend constexpr bool operator<=(int, weak_ordering a) noexcept { return 0 <= a.v_; }
    friend constexpr bool operator>=(int, weak_ordering a) noexcept { return 0 >= a.v_; }
    friend constexpr weak_ordering operator<=>(weak_ordering a, int) noexcept { return a; }
    friend constexpr weak_ordering operator<=>(int, weak_ordering a) noexcept {
        return weak_ordering(static_cast<signed char>(-a.v_));
    }
};
inline constexpr weak_ordering weak_ordering::less(-1);
inline constexpr weak_ordering weak_ordering::equivalent(0);
inline constexpr weak_ordering weak_ordering::greater(1);

class strong_ordering {
    signed char v_;
    constexpr explicit strong_ordering(signed char v) noexcept : v_(v) {}

public:
    static const strong_ordering less, equal, equivalent, greater;
    constexpr operator partial_ordering() const noexcept {
        if (v_ < 0) return partial_ordering::less;
        if (v_ > 0) return partial_ordering::greater;
        return partial_ordering::equivalent;
    }
    constexpr operator weak_ordering() const noexcept {
        if (v_ < 0) return weak_ordering::less;
        if (v_ > 0) return weak_ordering::greater;
        return weak_ordering::equivalent;
    }
    friend constexpr bool operator==(strong_ordering a, strong_ordering b) noexcept { return a.v_ == b.v_; }
    friend constexpr bool operator==(strong_ordering a, int) noexcept { return a.v_ == 0; }
    friend constexpr bool operator<(strong_ordering a, int) noexcept { return a.v_ < 0; }
    friend constexpr bool operator>(strong_ordering a, int) noexcept { return a.v_ > 0; }
    friend constexpr bool operator<=(strong_ordering a, int) noexcept { return a.v_ <= 0; }
    friend constexpr bool operator>=(strong_ordering a, int) noexcept { return a.v_ >= 0; }
    friend constexpr bool operator<(int, strong_ordering a) noexcept { return 0 < a.v_; }
    friend constexpr bool operator>(int, strong_ordering a) noexcept { return 0 > a.v_; }
    friend constexpr bool operator<=(int, strong_ordering a) noexcept { return 0 <= a.v_; }
    friend constexpr bool operator>=(int, strong_ordering a) noexcept { return 0 >= a.v_; }
    friend constexpr strong_ordering operator<=>(strong_ordering a, int) noexcept { return a; }
    friend constexpr strong_ordering operator<=>(int, strong_ordering a) noexcept {
        return strong_ordering(static_cast<signed char>(-a.v_));
    }
};
inline constexpr strong_ordering strong_ordering::less(-1);
inline constexpr strong_ordering strong_ordering::equal(0);
inline constexpr strong_ordering strong_ordering::equivalent(0);
inline constexpr strong_ordering strong_ordering::greater(1);

}  // namespace std
