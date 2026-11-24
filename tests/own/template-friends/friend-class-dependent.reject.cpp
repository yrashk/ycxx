// [class.friend]/3, [temp.inst]: a class template's friend `Peer<T>` befriends, in Box<int>, only
// Peer<int>; Peer<long> may not name Box<int>'s private member.
template <class T> class Peer;

template <class T>
class Box {
    friend Peer<T>;
    T v = 0;
};

template <class T>
class Peer {
public:
    static int get(const Box<int>& b) { return b.v; } //! error
};

int main() { return Peer<int>::get(Box<int>()) + Peer<long>::get(Box<int>()); }
