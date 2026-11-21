#ifndef IMPLICIT_CTOR_ALIASED_MOVE_ASSIGNMENT_HPP
#define IMPLICIT_CTOR_ALIASED_MOVE_ASSIGNMENT_HPP

struct AliasedMove {
    using Self = AliasedMove;

    int value;
    AliasedMove(int input) : value(input) {}
    void operator=(const volatile Self&&);
};

#endif
