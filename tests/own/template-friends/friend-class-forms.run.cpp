// [temp.friend]/1, [class.friend]/2-3, [dcl.type.elab]/4: the forms of class friends libycxx's
// headers use. A qualified friend class template; one with a constrained head; a class template
// befriending its own template ([temp.local]/1); and a class template's friend classes, a
// qualified one and a dependent specialization, which each specialization of it befriends.
namespace detail {
template <class T> struct Access;
struct Tag;
}

namespace lib {
template <class T>
concept small = sizeof(T) <= 8;

template <class T>
    requires small<T>
class Gate;

class Store {
    template <class T>
    friend struct detail::Access;
    template <class T>
        requires small<T>
    friend class Gate;
    int value = 3;
};

template <class T>
    requires small<T>
class Gate {
public:
    static int open(const Store& s) { return s.value; }
};

// Each specialization of Cell may read every other's private member.
template <class T>
class Cell {
    template <class U>
    friend class Cell;
    T v;

public:
    Cell(T x) : v(x) {}
    // Qualified until the injected-class-name with arguments parses here (#1177).
    template <class U>
    T from(const lib::Cell<U>& o) const { return T(o.v); }
};

template <class T> class Box;

template <class T>
class Peer {
public:
    static T get(const Box<T>& b) { return b.v; }
};

template <class T>
class Box {
    friend detail::Tag;
    friend struct detail::Access<T>;
    friend Peer<T>;
    T v;

public:
    Box(T x) : v(x) {}
};
}

namespace detail {
template <class T>
struct Access {
    static int read(const lib::Store& s) { return s.value; }
    static T box(const lib::Box<T>& b) { return b.v; }
};
struct Tag {
    static long read(const lib::Box<long>& b) { return b.v; }
};
}

int main() {
    lib::Store s;
    if (detail::Access<char>::read(s) != 3) return 1;
    if (lib::Gate<int>::open(s) != 3) return 2;
    if (lib::Cell<int>(0).from(lib::Cell<long>(9)) != 9) return 3;
    if (lib::Peer<int>::get(lib::Box<int>(4)) != 4) return 4;
    if (detail::Tag::read(lib::Box<long>(5)) != 5) return 5;
    if (detail::Access<short>::box(lib::Box<short>(6)) != 6) return 6;
    return 0;
}
