// [expr.type.conv]/1, [expr.prim.req.simple], [expr.prim.req.compound] (#286): decay-copy of a
// dependent operand in a requires-expression, as the ranges CPOs write it.
template<class T, class U> concept same_as = __is_same(T, U);
template<class T> concept pointer = __is_pointer(T);

struct Iterator { int* p; };
struct Range {
    Iterator begin();
    int* data();
    int (&array())[4];
    int function(int);
    int (*callback())(int);
};
struct NoCopy {
    NoCopy() = default;
    NoCopy(const NoCopy&) = delete;
};
struct ReturnsReference { NoCopy& begin(); };
struct ReturnsPrvalue { NoCopy begin(); };
struct NoBegin {};

template<class T>
concept has_begin = requires(T& t) { { auto(t.begin()) } -> same_as<Iterator>; };
template<class T>
concept copyable_begin = requires(T& t) { auto(t.begin()); };
template<class T>
concept braced_begin = requires(T& t) { { auto{t.begin()} }; };
template<class T>
concept pointer_data = requires(T& t) { { auto(t.data()) } -> pointer; };
template<class T>
concept array_decays = requires(T& t) { { auto(t.array()) } -> same_as<int*>; };
template<class T>
concept callback_decays = requires(T& t) { { auto(t.callback()) } -> same_as<int (*)(int)>; };

static_assert(has_begin<Range>);
static_assert(!has_begin<NoBegin>);
static_assert(copyable_begin<Range>);
static_assert(!copyable_begin<ReturnsReference>);
static_assert(copyable_begin<ReturnsPrvalue>);
static_assert(braced_begin<Range>);
static_assert(!braced_begin<ReturnsReference>);
static_assert(pointer_data<Range>);
static_assert(!pointer_data<NoBegin>);
static_assert(array_decays<Range>);
static_assert(callback_decays<Range>);

// A requires-expression outside a concept.
template<class T>
constexpr bool begins = requires(T& t) { auto(t.begin()); };
static_assert(begins<Range>);
static_assert(!begins<NoBegin>);

// The reduction from #286.
template<class T> concept C = requires(T t) { { auto(t.begin()) }; };
static_assert(C<Range>);
static_assert(!C<int>);

// A constrained template selects on it.
template<class T> requires copyable_begin<T> constexpr int pick() { return 1; }
template<class T> constexpr int pick() { return 2; }
static_assert(pick<Range>() == 1);
static_assert(pick<ReturnsReference>() == 2);
