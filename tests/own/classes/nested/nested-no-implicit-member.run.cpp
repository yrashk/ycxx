// #306 [class.nest]/1: a nested class has no special relationship to the enclosing class's objects; it is a distinct type with its own object.
struct E {
    int x = 1;
    struct N { int x = 2; };
    N n;
};
static_assert(sizeof(E) == 2 * sizeof(int));
int main() { E e; return (e.x == 1 && e.n.x == 2) ? 0 : 1; }
