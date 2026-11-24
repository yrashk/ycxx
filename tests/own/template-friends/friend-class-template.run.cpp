// [temp.friend]/1, [class.friend]/2: `template <…> friend class X;` makes every specialization of
// class template X a friend, whether instantiated from its primary template or from a partial
// specialization, in a class and in a class template. Their member functions and member templates
// may name the befriending class's private members.
template <class C> class Reader;

class Account {
    template <class C>
    friend class Reader;
    int balance = 7;

public:
    virtual ~Account();
    int get() const noexcept { return balance; }
    Account() {}
};
Account::~Account() {}

template <class C>
class Reader {
public:
    int peek(const Account& a) noexcept { return a.balance + int(sizeof(C)) - int(sizeof(C)); }
    template <class U>
    U as(const Account& a) noexcept { return U(a.balance); }
};

template <class C>
class Reader<C*> {
public:
    int peek(const Account& a) noexcept { return a.balance + 1; }
};

// The friend declaration is the template's first: a later declaration names the same template.
namespace bank {
class Vault {
    template <class K, int N>
    friend struct Key;
    int code = 40;
};

template <class K, int N>
struct Key {
    static int open(const Vault& v) { return v.code + N; }
};
}

// A class template befriending a class template: each specialization of Box befriends every
// specialization of Peek, its members' types dependent or not.
template <class T>
class Box {
    template <class U>
    friend class Peek;
    T value;

public:
    Box(T v) : value(v) {}
};

template <class U>
class Peek {
public:
    template <class T>
    static T look(const Box<T>& b) { return b.value; }
    static U first(const Box<U>& b) { return b.value; }
};

// The calls do not throw while `a` is alive: a cleanup on unwinding is a separate gap on Linux (#1104).
int account() {
    Account a;
    if (Reader<int>{}.peek(a) + a.get() != 14) return 1;
    if (Reader<char>{}.as<long>(a) != 7) return 2;
    if (Reader<int*>{}.peek(a) != 8) return 4;
    return 0;
}

int main() {
    if (int r = account()) return r;
    if (bank::Key<int, 2>::open(bank::Vault{}) != 42) return 5;
    if (Peek<int>::look(Box<long>(5)) != 5) return 6;
    if (Peek<char>::first(Box<char>('a')) != 'a') return 7;
    return 0;
}
