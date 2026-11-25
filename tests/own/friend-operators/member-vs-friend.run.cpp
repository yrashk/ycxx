// #1178, [over.match.oper], [over.match.best]: overload resolution between a member operator
// and a hidden friend; either may be strictly better.
// EXIT: 0
struct FriendBetter {
    int operator~() const { return 1; }
    friend int operator~(FriendBetter&) { return 2; }
    int operator++() const { return 1; }
    friend int operator++(FriendBetter&) { return 2; }
    int operator+=(long) { return 1; }
    friend int operator+=(FriendBetter&, int) { return 2; }
    int operator*=(const FriendBetter&) const { return 1; }
    friend int operator*=(FriendBetter&, const FriendBetter&) { return 2; }
    int operator!() const& { return 1; }
    friend int operator!(FriendBetter&&) { return 2; }
};
struct MemberBetter {
    int operator~() { return 1; }
    friend int operator~(const MemberBetter&) { return 2; }
    int operator--() { return 1; }
    friend int operator--(const MemberBetter&) { return 2; }
    int operator-=(int) { return 1; }
    friend int operator-=(MemberBetter&, double) { return 2; }
    int operator*() && { return 1; }
    friend int operator*(const MemberBetter&) { return 2; }
    int operator&=(const MemberBetter&) { return 1; }
    template <class T> friend int operator&=(MemberBetter&, const T&) { return 2; }
};
struct Base {
    int operator+() const { return 1; }
    int operator|=(int) { return 1; }
};
struct Derived : Base {
    friend int operator+(const Derived&) { return 2; }
    friend int operator|=(Derived&, int) { return 2; }
};
struct Base2 {
    friend int operator!(const Base2&) { return 2; }
    friend int operator^=(Base2&, int) { return 2; }
};
struct Derived2 : Base2 {
    int operator!() const { return 1; }
    int operator^=(int) { return 1; }
};
struct Inside {
    int operator!() const { return 1; }
    // The members of Inside do not hide the friends of another class's operands.
    int use(Base2& b) const { return !b; }
    int operator^=(Inside) { return 1; }
    int use2(Base2& b) const { return b ^= 1; }
};

int main() {
    FriendBetter f;
    const FriendBetter& cf = f;
    if (~f != 2 || ~cf != 1 || ~FriendBetter{} != 1) return 1;
    if (++f != 2 || ++cf != 1) return 2;
    if ((f += 1) != 2 || (f += 1L) != 1) return 3;
    if ((f *= f) != 2 || (FriendBetter{} *= f) != 1) return 4;
    if (!f != 1 || !FriendBetter{} != 2) return 5;
    MemberBetter m;
    const MemberBetter c{};
    if (~m != 1 || ~c != 2 || ~MemberBetter{} != 1) return 6;
    if (--m != 1 || --c != 2) return 7;
    if ((m -= 1) != 1 || (m -= 1.0) != 2) return 8;
    if (*m != 2 || *MemberBetter{} != 1) return 9;
    if ((m &= m) != 1 || (m &= 2) != 2) return 10;
    Derived d;
    if (+d != 2 || (d |= 1) != 2) return 11;
    Derived2 d2;
    if (!d2 != 1 || (d2 ^= 1) != 1) return 12;
    Base2 b2;
    return Inside().use(b2) == 2 && Inside().use2(b2) == 2 ? 0 : 13;
}
