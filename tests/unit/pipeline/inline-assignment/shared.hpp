#ifndef IMPLICIT_ASSIGNMENT_SHARED_HPP
#define IMPLICIT_ASSIGNMENT_SHARED_HPP

struct L {
    int value;
    L& operator=(const L&);
};

struct H {
    L member;
};

int assign_in_peer(int input);

#endif
