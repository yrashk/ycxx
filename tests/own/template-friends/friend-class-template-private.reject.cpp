// [temp.friend]/1, [class.access]: a friend class template's specializations may name the class's
// private members; a class that is not one of them may not, nor a class template of the same name
// in another namespace ([dcl.type.elab]/4: the friend's lookup stops at the innermost enclosing
// namespace, so the friend is a template of that namespace).
template <class C> class Outside;

namespace n {
template <class C> class Reader;

class Account {
    template <class C>
    friend class Reader;
    template <class C>
    friend class Outside;
    int balance = 7;
};

template <class C>
class Reader {
public:
    int peek(const Account& a) { return a.balance; }
};

class Auditor {
public:
    int peek(const Account& a) { return a.balance; } //! error
};

template <class C>
class Snoop {
public:
    int peek(const Account& a) { return a.balance; } //! error
};
}

template <class C>
class Outside {
public:
    int peek(const n::Account& a) { return a.balance; } //! error
};

int main() {
    n::Account a;
    return n::Reader<int>{}.peek(a) + n::Snoop<int>{}.peek(a) + Outside<int>{}.peek(a);
}
