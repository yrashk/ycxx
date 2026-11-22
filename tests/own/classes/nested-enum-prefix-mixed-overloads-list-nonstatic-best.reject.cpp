// [over.ics.rank] prefers the standard null pointer conversion to the user-defined sequence.
struct A { int n; };

struct Owner {
    long long f(long*);
    static int f(A);
    enum Result { width = sizeof(f({0})) }; //! error
};
