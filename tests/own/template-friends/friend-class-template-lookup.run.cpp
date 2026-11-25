// [temp.friend]/1, [dcl.type.elab]/4, [namespace.udecl]: a friend class template's unqualified
// name is looked up in the innermost enclosing namespace, which includes what its
// using-declarations introduce; a template it first declares, from a class template and with a
// non-type parameter whose type is another of its parameters, is the one declared there later.
namespace m {
template <class> class Reader;
}
using m::Reader;

struct Account {
    template <class>
    friend class Reader;

private:
    int balance = 1;
};

namespace m {
template <class T>
class Reader {
public:
    int peek(const Account& a) { return a.balance; }
};
}

template <class T>
struct Holder {
    template <class U, U N>
    friend class Constant;

private:
    int value = 2;
};

template <class U, U N>
struct Constant {
    template <class H>
    int read(const H& h) { return h.value + int(N); }
};

int main() {
    if (m::Reader<int>{}.peek(Account{}) != 1) return 1;
    if (Constant<int, 3>{}.read(Holder<long>{}) != 5) return 2;
    return 0;
}
