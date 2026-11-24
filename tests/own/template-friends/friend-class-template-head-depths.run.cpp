// [temp.over.link]/6: equivalent heads can name their own and nested template parameters
// by different names and at different depths. The same renaming applies inside pointer types.
// EXIT: 0
template<class T, T N> struct Reader;
template<class T, template<class U, T*, U*> class W> struct Nested;

template<class Outer>
class Account {
    template<class U, U N> friend struct Reader;
    template<class A, template<class B, A*, B*> class W> friend struct Nested;
    int secret = 37;
};

template<class T, T N>
struct Reader {
    static int read(const Account<long>& a) { return a.secret; }
};

template<class T, template<class U, T*, U*> class W>
struct Nested {
    static int read(const Account<long>& a) { return a.secret; }
};

template<class U, int*, U*> struct Any;

int main() {
    return Reader<int, 0>::read(Account<long>{}) == 37 &&
           Nested<int, Any>::read(Account<long>{}) == 37 ? 0 : 1;
}
