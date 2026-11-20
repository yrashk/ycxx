#ifndef IMPLICIT_ASSIGNMENT_NONCONST_MEMBER_HPP
#define IMPLICIT_ASSIGNMENT_NONCONST_MEMBER_HPP

struct Leaf {
    int value;

    Leaf& operator=(Leaf& source) {
        value = source.value + 1;
        source.value += 2;
        return *this;
    }
};

struct Whole {
    Leaf member;
};

#endif
