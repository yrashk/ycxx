// #316: `T x(&y)` initializes x at namespace scope, for a class and for a deduced class type ([dcl.init.general], [dcl.type.class.deduct])
struct S {
    int* p;
    S(int* q) : p(q) {}
};
template <class T> struct W {
    T* p;
    W(T* q) : p(q) {}
};

int i = 3;
S s(&i);
W w(&i), v(&i);
int j = i + 1;

int main() { return *s.p + *w.p + *v.p + j - 13; }
