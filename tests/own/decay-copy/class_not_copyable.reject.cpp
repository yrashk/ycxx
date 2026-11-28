// [expr.type.conv]/1, [dcl.init.general]/16.6.2 (#286): a class lvalue is copied, so its copy
// constructor must be usable. A prvalue initializes the result directly.
struct NoCopy {
    NoCopy() = default;
    NoCopy(const NoCopy&) = delete;
};
NoCopy make();
NoCopy object;

void use() {
    (void)auto(make());
    (void)auto{make()};
    (void)auto(object);  //! error
    (void)auto{object};  //! error
}

template<class T>
void copy(T& t) {
    (void)auto(t);  //! error
}
void instantiate() { copy(object); }
