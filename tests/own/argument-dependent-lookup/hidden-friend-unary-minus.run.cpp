namespace numbers {
struct Value {
    int value;
    friend int operator-(const Value& operand) { return -operand.value; }
};
int operator-(Value& operand) { return operand.value + 1; }
}

namespace member_competition {
struct MemberWins {
    int operator-() & { return 11; }
    friend int operator-(const MemberWins&) { return 12; }
};

class FriendWins {
    int operator-() const & { return 13; }
    friend int operator-(FriendWins&) { return 14; }
};
}

namespace base {
struct A {};
int operator-(A&) { return 21; }

struct Member {
    int operator-() const { return 23; }
};
}
namespace derived {
struct Value : base::A {
    friend int operator-(const base::A&) { return 22; }
};
struct PrivateBase : private base::Member {
    friend int operator-(PrivateBase&) { return 24; }
};
struct Rvalue {
    int operator-() const && { return 25; }
    friend int operator-(const Rvalue&) { return 26; }
};
}

namespace related_bases {
struct Root {};
struct Near : Root { int operator-() { return 31; } };
struct SameTarget : Near {
    friend int operator-(const Near&) { return 32; }
};
struct RelatedTarget : Near {
    friend int operator-(const Root&) { return 33; }
};
int same(SameTarget& value) { return -value; }
int related(RelatedTarget& value) { return -value; }
}

namespace member_beats_ancestor {
struct A {};
struct B {};
struct MemberBase : A { int operator-() { return 41; } };
struct Value : MemberBase, B {
    friend int operator-(A&) { return 42; }
};
int use() {
    Value value;
    return -value;
}
}

namespace rvalue_base_order {
struct A {};
struct B : A { constexpr int operator-() const & { return 51; } };
struct Related : B { friend constexpr long operator-(A&&) { return 52; } };
struct Other {};
struct Unrelated : B, Other { friend constexpr long operator-(Other&&) { return 53; } };
static_assert(__is_same(decltype(-Related{}), int));
static_assert(__is_same(decltype(-Unrelated{}), long));
}

namespace nonviable_template {
struct Other {};
template<class T> int operator-(Other&);
template<class T = int> int operator-(const Other&);
struct Value {
    template<class T> int operator-();
    friend constexpr int operator-(Value&) { return 61; }
};
constexpr int use() {
    Value value;
    return -value;
}

}

int main() {
    numbers::Value value{42};
    member_competition::MemberWins member;
    member_competition::FriendWins nonmember;
    derived::Value derived_value;
    derived::PrivateBase private_base;
    related_bases::SameTarget same_target;
    related_bases::RelatedTarget related_target;
    int builtin = 7;
    return -numbers::Value{42} == -42 && -value == 43 &&
           -member == 11 && -nonmember == 14 && -builtin == -7 &&
           -derived_value == 21 && -private_base == 24 && -derived::Rvalue{} == 25 &&
           related_bases::same(same_target) == 31 && related_bases::related(related_target) == 31 &&
           member_beats_ancestor::use() == 41 && -rvalue_base_order::Related{} == 51 &&
           -rvalue_base_order::Unrelated{} == 53 && nonviable_template::use() == 61 ? 0 : 1;
}
