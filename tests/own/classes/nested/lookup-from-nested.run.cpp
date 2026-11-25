// #306 [class.nest]/1, [basic.lookup.unqual]: enclosing static members, enumerators, types and aliases are found unqualified.
class E {
    static int sm;
    enum Color { Red = 3, Blue = 5 };
    typedef int Count;
    using Num = long;
    struct Other { int q; };
    static int sf() { return 100; }
public:
    struct N {
        int body() {
            Count c = Red;
            Num n = Blue;
            Other o{1};
            return sm + c + (int)n + o.q + sf();
        }
        int inbody_sizeof = sizeof(Count);
        Color col = Blue;
    };
    int run() { N n; return n.body() + (n.col == Blue ? 1000 : 0); }
};
int E::sm = 1;
int main() { E e; return e.run() == 1110 ? 0 : 1; }
