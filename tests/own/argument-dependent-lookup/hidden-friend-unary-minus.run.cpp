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

int main() {
    numbers::Value value{42};
    member_competition::MemberWins member;
    member_competition::FriendWins nonmember;
    int builtin = 7;
    return -numbers::Value{42} == -42 && -value == 43 &&
           -member == 11 && -nonmember == 14 && -builtin == -7 ? 0 : 1;
}
