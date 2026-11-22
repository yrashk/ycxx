#ifndef ABI309_ROOT_HPP
#define ABI309_ROOT_HPP

namespace abi309 {
struct Root {
    int member;
    virtual int first();
    virtual int second();
};

int call_second(Root& root);
}

#endif
