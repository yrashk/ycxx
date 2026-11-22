#ifndef ABI309_DEFAULT_ROOT_HPP
#define ABI309_DEFAULT_ROOT_HPP

namespace abi309 {
struct Root {
    int count;
    virtual int first();
    virtual int second();
};

int make_root();
int peer_check(Root* root);
}

#endif
