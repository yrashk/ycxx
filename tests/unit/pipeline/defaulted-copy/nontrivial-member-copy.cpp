int member_copies = 0;

struct Member {
    int value;
    Member(int input) : value(input) {}

    Member(const Member& source) : value(source.value + 3) {
        ++member_copies;
    }
};

struct W {
    Member member;
    W(int input) : member(input) {}
    W(const W&) = default;
};

int main() {
    const W source(31);
    W copy(source);
    return member_copies == 1 && source.member.value == 31 && copy.member.value == 34 ? 0 : 1;
}
