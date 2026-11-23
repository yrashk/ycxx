#ifndef YCXX_1025_LOCAL_OWNER_HPP
#define YCXX_1025_LOCAL_OWNER_HPP

extern int destructions;

template<class Tag>
struct Member {
    ~Member() { destructions += Tag::amount; }
};

template<class Tag>
struct Owner {
    Member<Tag> member;
};

void destroy_first();

#endif
