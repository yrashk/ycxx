// #316: implicit guides from a constructor, and a user guide for an aggregate ([over.match.class.deduct]/1)
template <class T> struct Box { T v; Box(T x) : v(x) {} };
template <class T> struct Agg { T v; };
template <class T> Agg(T) -> Agg<T>;
int main() { Box b(3); Agg a{4}; return b.v + a.v - 7; }
