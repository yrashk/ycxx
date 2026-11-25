// #1178, [temp.friend], [temp.inst]/4: a class template's friend operator accesses only its own
// specialization's private members.
template <class T>
class Box {
    T v = 1;

public:
    friend T operator~(const Box& b) { return b.v; }
    friend Box& operator+=(Box& a, const Box& b) { a.v += b.v; return a; }
    template <class U> friend T operator*=(Box& a, const Box<U>& b) {
        return a.v * b.v; //! error
    }
};

int main() {
    Box<int> i;
    Box<long> l;
    i += i;
    i *= l;
    return ~i;
}
