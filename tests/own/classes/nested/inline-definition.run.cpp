// #306 [class.nest]/1, [class.mem]: a class defined in-class is a member; named S::In, aggregate-initialised, member functions in its body.
struct S {
    struct In {
        int w;
        int twice() const { return w * 2; }
        void set(int v) { w = v; }
    };
    In i;
};
int main() {
    S s{{5}};
    S::In a{7};
    if (s.i.w != 5 || a.twice() != 14) return 1;
    s.i.set(9);
    if (s.i.twice() != 18) return 2;
    return 0;
}
