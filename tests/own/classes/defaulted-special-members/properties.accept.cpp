// #157 [dcl.fct.def.default], [class.copy.ctor]/10, [class.copy.assign]/7, [class.default.ctor]/2,
// [except.spec]: what is deleted, and the implicit exception specifications.
template <class T> T&& value() noexcept;
template <class T> concept default_made = requires { T(); };
template <class T, class A> concept made_from = requires { T(value<A>()); };
template <class T, class A> concept assigned_from = requires { value<T>() = value<A>(); };

struct Plain {
  Plain() = default;
  Plain(const Plain&) = default;
  Plain(Plain&&) = default;
  Plain& operator=(const Plain&) = default;
  Plain& operator=(Plain&&) = default;
  int x;
};
static_assert(noexcept(Plain()) && noexcept(Plain(value<const Plain&>())) && noexcept(Plain(value<Plain>())));
static_assert(noexcept(value<Plain&>() = value<const Plain&>()) && noexcept(value<Plain&>() = value<Plain>()));

struct Throws {
  Throws();
  Throws(const Throws&);
  Throws(Throws&&) noexcept;
  Throws& operator=(const Throws&);
  Throws& operator=(Throws&&) noexcept;
};
template <class T>
struct Holds {
  Holds() = default;
  Holds(const Holds&) = default;
  Holds(Holds&&) = default;
  Holds& operator=(const Holds&) = default;
  Holds& operator=(Holds&&) = default;
  T t;
};
using HT = Holds<Throws>;
static_assert(!noexcept(HT()) && !noexcept(HT(value<const HT&>())) && noexcept(HT(value<HT>())));
static_assert(!noexcept(value<HT&>() = value<const HT&>()) && noexcept(value<HT&>() = value<HT>()));
using HI = Holds<int[3]>;
static_assert(noexcept(HI()) && noexcept(HI(value<const HI&>())) && noexcept(value<HI&>() = value<HI>()));

// A written specification is the specification, whatever the subobjects' are.
struct Written {
  Written(const Written&) noexcept = default;
  Written(Written&&) noexcept(false) = default;
  Throws t;
};
static_assert(noexcept(Written(value<const Written&>())) && !noexcept(Written(value<Written>())));

// So is a trivial member's, and it counts in the specification of a class that uses it.
struct MayThrow {
  MayThrow();
  MayThrow(const MayThrow&) noexcept(false) = default;
  MayThrow& operator=(const MayThrow&) noexcept(false) = default;
  int x;
};
static_assert(!noexcept(MayThrow(value<const MayThrow&>())) && !noexcept(value<MayThrow&>() = value<const MayThrow&>()));
struct HasMayThrow {
  HasMayThrow(const HasMayThrow&) = default;
  HasMayThrow& operator=(const HasMayThrow&) = default;
  MayThrow m[2];
};
static_assert(!noexcept(HasMayThrow(value<const HasMayThrow&>())));
static_assert(!noexcept(value<HasMayThrow&>() = value<const HasMayThrow&>()));
static_assert(__is_trivially_copyable(MayThrow));

// A subobject's destructor does not contribute to a constructor's specification.
struct ThrowingEnd {
  ThrowingEnd() noexcept;
  ThrowingEnd(const ThrowingEnd&) noexcept;
  ~ThrowingEnd() noexcept(false);
};
struct HasThrowingEnd {
  HasThrowingEnd() = default;
  HasThrowingEnd(const HasThrowingEnd&) = default;
  ThrowingEnd e;
};

struct NoCopy {
  NoCopy();
  NoCopy(const NoCopy&) = delete;
  NoCopy& operator=(const NoCopy&) = delete;
};
static_assert(made_from<Holds<int>, const Holds<int>&>);
static_assert(!made_from<Holds<NoCopy>, const Holds<NoCopy>&>);
static_assert(!made_from<Holds<NoCopy>, Holds<NoCopy>>);
static_assert(default_made<Holds<NoCopy>>);
static_assert(!assigned_from<Holds<NoCopy>&, const Holds<NoCopy>&>);
static_assert(!assigned_from<Holds<NoCopy>&, Holds<NoCopy>>);

struct MoveOnly {
  MoveOnly(MoveOnly&&);
  MoveOnly& operator=(MoveOnly&&);
};
static_assert(!default_made<Holds<MoveOnly>>);
static_assert(!made_from<Holds<MoveOnly>, const Holds<MoveOnly>&>);
static_assert(made_from<Holds<MoveOnly>, Holds<MoveOnly>>);
static_assert(!assigned_from<Holds<MoveOnly>&, const Holds<MoveOnly>&>);
static_assert(assigned_from<Holds<MoveOnly>&, Holds<MoveOnly>>);

// References and const members.
using HR = Holds<int&>;
static_assert(!default_made<HR> && made_from<HR, const HR&> && made_from<HR, HR>);
static_assert(!assigned_from<HR&, const HR&> && !assigned_from<HR&, HR>);
using HRR = Holds<int&&>;
static_assert(!made_from<HRR, const HRR&> && made_from<HRR, HRR>);
using HC = Holds<const int>;
static_assert(!default_made<HC> && made_from<HC, const HC&> && !assigned_from<HC&, const HC&>);

// Access and destructors.
class PrivateCopy {
  PrivateCopy(const PrivateCopy&);
  friend struct Friend;
public:
  PrivateCopy(int);
};
struct Stranger { Stranger(const Stranger&) = default; PrivateCopy p; };
struct Friend { Friend(const Friend&) = default; PrivateCopy p; };
static_assert(!made_from<Stranger, const Stranger&> && made_from<Friend, const Friend&>);
class PrivateEnd { ~PrivateEnd(); };
struct HasPrivateEnd { HasPrivateEnd() = default; HasPrivateEnd(const HasPrivateEnd&) = default; PrivateEnd p; };
static_assert(!made_from<HasPrivateEnd, const HasPrivateEnd&>);
struct ProtectedBase {
protected:
  ProtectedBase();
  ProtectedBase(const ProtectedBase&);
  ~ProtectedBase();
};
struct FromProtected : ProtectedBase {
  FromProtected() = default;
  FromProtected(const FromProtected&) = default;
};
static_assert(default_made<FromProtected> && made_from<FromProtected, const FromProtected&>);

// [dcl.fct.def.default]: a type that differs from the implicit declaration's deletes the function.
struct TakesMutable { TakesMutable(TakesMutable&); TakesMutable& operator=(TakesMutable&); };
struct Mismatched {
  Mismatched(const Mismatched&) = default;
  Mismatched& operator=(const Mismatched&) = default;
  TakesMutable t;
};
static_assert(!made_from<Mismatched, const Mismatched&> && !made_from<Mismatched, Mismatched&>);
static_assert(!assigned_from<Mismatched&, Mismatched&>);
struct Matched {
  Matched(Matched&) = default;
  Matched& operator=(Matched&) = default;
  TakesMutable t;
};
static_assert(made_from<Matched, Matched&> && !made_from<Matched, const Matched&>);
static_assert(assigned_from<Matched&, Matched&> && !assigned_from<Matched&, const Matched&>);
struct VolatileParameter { VolatileParameter(const volatile VolatileParameter&) = default; };
static_assert(!made_from<VolatileParameter, const VolatileParameter&>);
struct ConstMove { ConstMove(const ConstMove&&) = default; };
static_assert(!made_from<ConstMove, ConstMove>);
struct ConstAssign { ConstAssign& operator=(const ConstAssign&) const = default; };
static_assert(!assigned_from<const ConstAssign&, const ConstAssign&>);

// A volatile scalar member does not make an operation nontrivial or deleted ([class.copy.ctor]/11).
struct VolatileScalar {
  VolatileScalar() = default;
  VolatileScalar(const VolatileScalar&) = default;
  VolatileScalar& operator=(const VolatileScalar&) = default;
  volatile int x;
};
static_assert(__is_trivially_copyable(VolatileScalar));
static_assert(default_made<VolatileScalar> && made_from<VolatileScalar, const VolatileScalar&>);
static_assert(assigned_from<VolatileScalar&, const VolatileScalar&>);
// A volatile class member has no copy constructor that takes it.
struct VolatileClass {
  VolatileClass(const VolatileClass&) = default;
  volatile Plain p;
};
static_assert(!made_from<VolatileClass, const VolatileClass&>);

// Variant members ([class.default.ctor], [class.copy.ctor]).
union TrivialUnion {
  TrivialUnion() = default;
  TrivialUnion(const TrivialUnion&) = default;
  int i;
  float f;
};
static_assert(default_made<TrivialUnion> && made_from<TrivialUnion, const TrivialUnion&>);

// An implicit copy of a class with a defaulted one.
struct Outer { HT t; };
static_assert(made_from<Outer, const Outer&>);
struct OuterNoCopy { Holds<NoCopy> t; };
static_assert(!made_from<OuterNoCopy, const OuterNoCopy&>);
