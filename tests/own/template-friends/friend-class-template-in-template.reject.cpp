// [temp.friend]/1, [class.access]: a class template's friend class template befriends every
// specialization of that template from every specialization of the class; another class template
// is still not a friend.
template <class T>
class Box {
    template <class U>
    friend class Peek;
    T value = 0;
};

template <class U>
class Peek {
public:
    static int look(const Box<long>& b) { return int(b.value); }
};

template <class U>
class Other {
public:
    static int look(const Box<long>& b) { return int(b.value); } //! error
};

int main() { return Peek<int>::look(Box<long>()) + Other<int>::look(Box<long>()); }
