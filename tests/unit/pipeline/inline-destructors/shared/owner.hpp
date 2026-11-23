#ifndef YCXX_1025_EXPLICIT_OWNER_HPP
#define YCXX_1025_EXPLICIT_OWNER_HPP

extern int primary_destructions;
extern int specialized_destructions;

template<class Tag>
struct Member {
    ~Member() { ++primary_destructions; }
};

template<>
struct Member<int> {
    ~Member() { ++specialized_destructions; }
};

struct Owner {
    Member<int> specialized;
    Member<bool> ordinary;
};

void destroy_first();

#endif
