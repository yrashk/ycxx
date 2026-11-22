struct W;

struct Member {
    int value;
    Member(int input) : value(input) {}

private:
    Member(const Member&) = default;
    friend struct W;
};

struct W {
    Member member;
    W(int input) : member(input) {}
    W(const W&) = default;
};

int main() {
    const W source(31);
    W copy(source);
    return copy.member.value == 31 && &copy.member.value != &source.member.value ? 0 : 1;
}
