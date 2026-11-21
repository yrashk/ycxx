#ifndef DEFAULTED_INACCESSIBLE_MEMBER_COPY_HPP
#define DEFAULTED_INACCESSIBLE_MEMBER_COPY_HPP

struct Member {
    int value;
    Member(int input) : value(input) {}

private:
    Member(const Member&) = default;
};

struct W {
    Member member;
    W(int input) : member(input) {}
    W(const W&) = default;
};

#endif
