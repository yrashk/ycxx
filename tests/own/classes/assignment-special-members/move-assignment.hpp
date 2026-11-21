#ifndef IMPLICIT_CTOR_MOVE_ASSIGNMENT_HPP
#define IMPLICIT_CTOR_MOVE_ASSIGNMENT_HPP

struct MoveAssigned {
    int value;

    MoveAssigned(int input) : value(input) {}
    MoveAssigned& operator=(MoveAssigned&&);
};

#endif
