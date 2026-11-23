// #1024 [meta.unary.prop], [dcl.array], [temp.arg.nontype].
template<bool Bounded, bool Unbounded>
struct Bounds {
  static constexpr bool bounded = Bounded;
  static constexpr bool unbounded = Unbounded;
};

template<class T>
struct BoundsOf : Bounds<__is_bounded_array(T), __is_unbounded_array(T)> {};

using Row = int[3];
using OpenRow = int[];
using Matrix = int[2][3];
using OpenMatrix = int[][3];

static_assert(BoundsOf<Matrix>::bounded);
static_assert(BoundsOf<OpenMatrix>::unbounded);
static_assert(!BoundsOf<long>::bounded && !BoundsOf<long>::unbounded);
static_assert(BoundsOf<Row>::bounded && !BoundsOf<Row>::unbounded);
static_assert(!BoundsOf<OpenRow>::bounded && BoundsOf<OpenRow>::unbounded);

static_assert(BoundsOf<const Matrix>::bounded && !BoundsOf<const Matrix>::unbounded);
static_assert(!BoundsOf<const volatile OpenMatrix>::bounded &&
              BoundsOf<const volatile OpenMatrix>::unbounded);
static_assert(!BoundsOf<Row*>::bounded && !BoundsOf<Row*>::unbounded);
static_assert(!BoundsOf<const Matrix&>::bounded && !BoundsOf<const Matrix&>::unbounded);
static_assert(!BoundsOf<OpenMatrix&&>::bounded && !BoundsOf<OpenMatrix&&>::unbounded);

static_assert(!BoundsOf<Matrix>::unbounded);
static_assert(!BoundsOf<OpenMatrix>::bounded);
