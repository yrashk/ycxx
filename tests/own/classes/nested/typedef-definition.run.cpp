// #306 [class.nest], [dcl.typedef]/1: a typedef declaration in a class can define a member class.
struct O { typedef struct I { int v; } T; T t; };
int main() { O o{{2}}; O::I i{1}; return o.t.v + i.v - 3; }
