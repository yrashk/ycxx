// [basic.def.odr]/1: a friend a class template's specialization defines is the namespace's
// function; a namespace-scope definition of the same function redefines it.
namespace n {
template <class T> struct A;
int f(const A<int>&) { return 1; }
template <class T>
struct A {
    friend int f(const A&) { return 2; } //! error
};
}

int main() {
    n::A<int> a;
    return f(a);
}
