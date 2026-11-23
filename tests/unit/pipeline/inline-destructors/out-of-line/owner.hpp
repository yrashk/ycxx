#ifndef YCXX_1025_OWNER_HPP
#define YCXX_1025_OWNER_HPP

extern int destructions;

struct Owner {
    ~Owner();
};

void destroy_first();

#endif
