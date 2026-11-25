// #1178, [temp.dep.candidate], [temp.dep], [basic.lookup.argdep]: dependent operands whose class has
// no viable friend are diagnosed at instantiation, and non-dependent ones at the definition.
namespace n {
struct None {};
struct Const {
    friend Const& operator++(Const& c) { return c; }
};
struct Host {
    friend int operator~(const None&) { return 1; }
};
}
template <class T> void inv(T t) {
    ~t; //! error
}
template <class T> void inc(const T& t) {
    ++t; //! error
}
template <class T> void add(T t) {
    t += 1; //! error
}
template <class T> void fixed(T t, n::None n) {
    ~n; //! error
}
void use() {
    inv(n::None());
    inc(n::Const());
    add(n::None());
}
