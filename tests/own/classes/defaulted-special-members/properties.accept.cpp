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
static_assert(!made_from<VolatileClass, const VolatileClass&> && !made_from<VolatileClass, VolatileClass>);
// Nor has an implicit operation, in the class itself or in one that contains it.
struct Aggregate { int x; };
struct ImplicitVolatile { volatile Aggregate p; };
struct ContainsVolatile { ContainsVolatile(const ContainsVolatile&) = default; ImplicitVolatile i; };
struct DerivedVolatile : ImplicitVolatile { DerivedVolatile() = default; DerivedVolatile(const DerivedVolatile&) = default; };
struct ArrayVolatile { ArrayVolatile() = default; volatile Plain p[2]; };
static_assert(!made_from<ImplicitVolatile, const ImplicitVolatile&> && !made_from<ImplicitVolatile, ImplicitVolatile>);
static_assert(!made_from<ContainsVolatile, const ContainsVolatile&>);
static_assert(!made_from<DerivedVolatile, const DerivedVolatile&> && !made_from<DerivedVolatile, DerivedVolatile>);
static_assert(!made_from<ArrayVolatile, const ArrayVolatile&> && !made_from<ArrayVolatile, ArrayVolatile>);
static_assert(!assigned_from<ImplicitVolatile&, const ImplicitVolatile&>);
// One that takes volatile objects is selected.
struct TakesVolatile {
  TakesVolatile();
  TakesVolatile(const volatile TakesVolatile&);
};
struct UsesVolatile { UsesVolatile() = default; UsesVolatile(const UsesVolatile&) = default; volatile TakesVolatile t; };
static_assert(made_from<UsesVolatile, const UsesVolatile&>);
// A volatile object is not copied by an implicit copy constructor.
static_assert(!made_from<Aggregate, volatile Aggregate&>);
// The implicit assignment of a class with a volatile scalar.
struct ImplicitScalar { volatile int x; };
struct HasImplicitScalar { HasImplicitScalar& operator=(const HasImplicitScalar&) = default; ImplicitScalar i; };
static_assert(assigned_from<ImplicitScalar&, const ImplicitScalar&>);
static_assert(assigned_from<HasImplicitScalar&, const HasImplicitScalar&>);

// A class whose only copy or move operation is a trivial move.
struct TrivialMove { TrivialMove() = default; TrivialMove(TrivialMove&&) = default; int x; };
struct HasTrivialMove { HasTrivialMove() = default; HasTrivialMove(HasTrivialMove&&) = default; TrivialMove t; };
static_assert(made_from<TrivialMove, TrivialMove> && !made_from<TrivialMove, const TrivialMove&>);
static_assert(made_from<HasTrivialMove, HasTrivialMove> && !made_from<HasTrivialMove, const HasTrivialMove&>);

// A constructor whose other parameters have no default arguments is no move constructor ([class.copy.ctor]/2).
struct NotMove { int&& r; NotMove(NotMove&&, int); };
struct NotMoveMember { TrivialMove t; NotMoveMember(); NotMoveMember(NotMoveMember&&, int); };
static_assert(made_from<NotMove, NotMove> && !made_from<NotMove, NotMove&>);
static_assert(made_from<NotMoveMember, NotMoveMember> && !made_from<NotMoveMember, const NotMoveMember&>);
// An implicit move of a class whose member's defaulted move is inaccessible, or takes no const member.
class PrivateMove { PrivateMove(PrivateMove&&) = default; int x; public: PrivateMove(); };
struct HasPrivateMove { HasPrivateMove(); PrivateMove m; };
struct HasConstMove { HasConstMove(); const TrivialMove m; };
static_assert(!made_from<HasPrivateMove, HasPrivateMove> && !made_from<HasConstMove, HasConstMove>);
// A volatile object of a derived class.
struct FromAggregate : Aggregate {};
static_assert(!made_from<Aggregate, volatile FromAggregate&>);
// Assignments that are deleted leave a requirement unsatisfied, whether or not ycxx supports the class.
template <class T> struct Wraps { T t; };
struct HasReference { int& r; };
struct DeletedAssign { DeletedAssign& operator=(const DeletedAssign&) = delete; };
struct DerivesDeleted : DeletedAssign { int z; };
static_assert(!assigned_from<Wraps<HasReference>&, const Wraps<HasReference>&>);
static_assert(!assigned_from<DerivesDeleted&, const DerivesDeleted&>);
struct UserAssign { UserAssign& operator=(const UserAssign&); };
struct DerivesUser : UserAssign {};
template <class T> requires requires(T& a) { a = a; } char (&preferred(T&))[2];
char (&preferred(DerivesUser&))[1];
static_assert(sizeof(preferred(value<DerivesUser&>())) == 1);

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
