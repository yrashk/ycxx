// #306 [class.nest]/1: the nested class name is found by qualified lookup and is a distinct type from a same-named outer-scope class.
struct In { int a; };
struct S { struct In { int a; int b; }; };
static_assert(sizeof(S::In) == 2 * sizeof(int));
static_assert(sizeof(In) == sizeof(int));
