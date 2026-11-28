// #316: a user deduction-guide at namespace scope deduces the class template's arguments ([temp.deduct.guide], [over.match.class.deduct])
template <class T> struct W {
    T* p;
    W(T& r) : p(&r) {}
};
template <class T> W(T&) -> W<T>;

int i = 5;
W w(i);

int main() {
    W local(i);
    *local.p += 1;
    return *w.p == 6 ? 0 : 1;
}
