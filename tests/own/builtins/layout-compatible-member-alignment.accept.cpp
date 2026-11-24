// XFAIL: clang Clang 23 does not compare the alignment of corresponding members' types.
// #218: [class.mem.general]: corresponding entities of a common initial sequence have the same
// alignment requirements. CA and I1 are layout-compatible, but their alignments differ, so structs
// holding them are not. Unions pair their members by type alone.
struct alignas(16) CA { int a; };
struct I1 { int a; };
struct HCA { CA c; };
struct HCA2 { CA c; };
struct HI { I1 c; };
union UA { CA c; };
union UI { I1 c; };
static_assert(__is_layout_compatible(CA, I1) && __is_layout_compatible(HCA, HCA2));
static_assert(!__is_layout_compatible(HCA, HI));
static_assert(__is_layout_compatible(UA, UI));
