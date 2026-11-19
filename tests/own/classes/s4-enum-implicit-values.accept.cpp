// #220, from the review: an implicit enumerator whose value its predecessor's type cannot
// represent has a type that can ([dcl.enum]/5), so its value is right inside the list.
enum P { p0 = 0x7FFFFFFF, p1, p2 = (p1 > 0) + 10 };
static_assert(p2 == 11);
enum Q { q0 = 0xFFFFFFFFu, q1, q2 = (q1 > 0xFFFFFFFFu) + 10 };
static_assert(q2 == 11);
enum S { s0 = 0x8000000000000000ULL, s1, s2 = (s1 > 0) + 10 };
static_assert(s2 == 11);

