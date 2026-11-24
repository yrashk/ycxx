// #306 [class.nest]/3, [class.mem]: member class declared in-class, defined at namespace scope.
class O {
public:
    class I;
    int use(const I&);
};
class O::I {
public:
    int x = 6;
    int get() const { return x; }
};
int O::use(const I& i) { return i.get() + 1; }
int main() { O o; O::I i; return o.use(i) == 7 ? 0 : 1; }
