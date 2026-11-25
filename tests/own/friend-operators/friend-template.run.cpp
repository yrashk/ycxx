// #1178, [temp.friend]/1, [temp.friend]/4, [over.match.best]: friend function templates defined in
// a class and in a class template, found only by argument-dependent lookup.
// EXIT: 0
namespace n {
class Log {
    int sum = 0;

public:
    constexpr int get() const { return sum; }
    template <class T> friend constexpr Log& operator<<=(Log& l, const T& t) { l.sum += int(t) * 10; return l; }
    friend constexpr Log& operator<<=(Log& l, char) { l.sum += 1; return l; }
    template <class T> friend constexpr Log& operator+=(Log& l, T* p) { l.sum += *p; return l; }
    template <class T> friend constexpr Log& operator+=(Log& l, T) { l.sum += 1000; return l; }
    template <int N> friend constexpr int operator-=(Log&, const int (&)[N]) { return N; }
};

template <class T>
struct Wrap {
    T v;
    template <class U> friend constexpr Wrap& operator+=(Wrap& a, const Wrap<U>& b) { a.v += T(b.v); return a; }
    template <class U> friend constexpr Wrap& operator*=(Wrap& a, U u) { a.v *= T(u); return a; }
};

struct Any {
    template <class T> friend constexpr int operator~(const T&) { return sizeof(T) + 40; }
    template <class T> friend constexpr int operator++(T&&) { return __is_same(T, T&) ? 1 : 2; }
};
struct Derived : Any { char pad[3]; };
}

constexpr int check() {
    n::Log l;
    l <<= 2;
    if (l.get() != 20) return 1;
    (l <<= 'c') <<= 3L;
    if (l.get() != 51) return 2;
    int i = 4;
    l += &i;
    if (l.get() != 55) return 3;
    l += i;
    if (l.get() != 1055) return 4;
    int arr[6] = {};
    if ((l -= arr) != 6) return 5;

    n::Wrap<int> w{2};
    n::Wrap<long> q{5};
    (w += q) += w;
    if (w.v != 14) return 6;
    q += w;
    if (q.v != 19) return 7;
    if (((w *= 2) *= 1.0).v != 28) return 8;

    if (~n::Any{} != 41 || ~n::Derived{} != 43) return 9;
    n::Any a;
    if (++a != 1 || ++n::Any{} != 2) return 10;
    return 0;
}
int main() { return check(); }
