template<int N>
struct Tag {};

template<class T>
struct Outer {};

extern Outer<Tag<7>> nested;
extern Outer<Tag<auto(7)>> nested;
