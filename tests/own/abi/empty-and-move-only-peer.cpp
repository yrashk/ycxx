// The other side of empty-and-move-only.run.cpp, compiled by Clang.
struct E {};
struct MV2 { int v; MV2(int x) : v(x) {} MV2(MV2&& o) : v(o.v) {} };
struct DEL { int v; DEL(int x) : v(x) {} DEL(const DEL&) = delete; DEL(DEL&&) = default; };
struct HasMV2 { MV2 m; int k; };
int peer_has(HasMV2 h) { return h.m.v * 10 + h.k; }
int peer_e(int a, E e, int b) { return a * 10 + b; }
E peer_make_e(int) { return E{}; }
int peer_mv2(MV2 m, int k) { return m.v * 10 + k; }
int peer_del(DEL d, int k) { return d.v * 10 + k; }
MV2 peer_r_mv2() { return MV2(5); }
int main_e(int a, E e, int b);
int peer_calls_e() { return main_e(1, E{}, 2); }
