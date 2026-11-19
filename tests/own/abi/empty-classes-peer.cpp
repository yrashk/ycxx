// The other side of empty-classes.run.cpp, compiled by Clang.
struct E {};
int peer_e(int a, E e, int b) { return a * 10 + b; }
E peer_make_e(int) { return E{}; }
int main_e(int a, E e, int b);
int peer_calls_e() { return main_e(1, E{}, 2); }
