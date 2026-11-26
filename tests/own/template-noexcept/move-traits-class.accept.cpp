// #218, [meta.unary.prop], [except.spec], [temp.inst]
// Both move operations are viable for each class. Their written specifications differ.
struct SafeMove {
  SafeMove(SafeMove&&) noexcept;
  SafeMove& operator=(SafeMove&&) noexcept;
  ~SafeMove() noexcept;
};

struct ThrowingConstruction {
  ThrowingConstruction(ThrowingConstruction&&) noexcept(false);
  ThrowingConstruction& operator=(ThrowingConstruction&&) noexcept;
  ~ThrowingConstruction() noexcept;
};

struct ThrowingAssignment {
  ThrowingAssignment(ThrowingAssignment&&) noexcept;
  ThrowingAssignment& operator=(ThrowingAssignment&&) noexcept(false);
  ~ThrowingAssignment() noexcept;
};

template<class T>
void move_construct(T&) noexcept(__is_nothrow_constructible(T, T&&));

template<class T>
void move_assign(T&) noexcept(__is_nothrow_assignable(T&, T&&));

template<class T>
void move_between(T&, T&) noexcept(
    __is_nothrow_constructible(T, T&&) && __is_nothrow_assignable(T&, T&&));

extern ThrowingConstruction throwing_construction;
extern SafeMove safe_move;
extern ThrowingAssignment throwing_assignment;

static_assert(!noexcept(move_construct(throwing_construction)));
static_assert(noexcept(move_assign(throwing_construction)));
static_assert(!noexcept(move_between(throwing_construction, throwing_construction)));
static_assert(noexcept(move_construct(safe_move)));
static_assert(noexcept(move_assign(safe_move)));
static_assert(noexcept(move_between(safe_move, safe_move)));
static_assert(noexcept(move_construct(throwing_assignment)));
static_assert(!noexcept(move_assign(throwing_assignment)));
static_assert(!noexcept(move_between(throwing_assignment, throwing_assignment)));
