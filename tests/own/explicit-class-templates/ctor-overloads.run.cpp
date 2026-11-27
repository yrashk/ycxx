// #715: copy-initialization of W<int> skips the explicit W(T) and uses the converting W(long); direct-initialization picks the exact W(T) ([over.match.copy], [over.match.ctor])
// EXIT: 0
template <class T> struct W {
    int tag;
    explicit W(T) : tag(1) {}
    W(long) : tag(2) {}
};

int take(W<int> w) { return w.tag; }
W<int> give() { return 7; }

int main() {
    W<int> direct(1);
    if (direct.tag != 1) return 1;
    W<int> list{1};
    if (list.tag != 1) return 2;
    W<int> copied = 1;
    if (copied.tag != 2) return 3;
    if (take(1) != 2) return 4;
    if (give().tag != 2) return 5;
    if (static_cast<W<int>>(1).tag != 1) return 6;
    return 0;
}
