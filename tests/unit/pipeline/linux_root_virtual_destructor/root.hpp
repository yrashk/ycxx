#ifndef YCXX_SPEC_ROOT_VIRTUAL_DTOR_HPP
#define YCXX_SPEC_ROOT_VIRTUAL_DTOR_HPP

class R {
public:
    virtual int key() noexcept;
    virtual ~R() noexcept;
};

void observe(R*) noexcept;
int call_key(R*) noexcept;
void destroy(R*) noexcept;

#endif
