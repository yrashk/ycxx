#ifndef IMPLICIT_CTOR_TEMPLATE_SHAPES_HPP
#define IMPLICIT_CTOR_TEMPLATE_SHAPES_HPP

struct R {
    int value;

    template<class Tag = void>
    R(R&&);
};

struct RWithMoveAssignment {
    int value;

    template<class Tag = void>
    RWithMoveAssignment(RWithMoveAssignment&&);

    RWithMoveAssignment& operator=(RWithMoveAssignment&&);
};

#endif
