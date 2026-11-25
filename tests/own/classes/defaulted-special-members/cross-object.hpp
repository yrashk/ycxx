// Classes whose layout or calling convention depends on their defaulted special members.
struct Triple {
  Triple() = default;
  Triple(const Triple&) = default;
  Triple(Triple&&) = default;
  Triple& operator=(const Triple&) = default;
  Triple& operator=(Triple&&) = default;
  ~Triple() = default;
  long a;
  long b;
};

struct Counted {
  int v;
  Counted(int x) : v(x) {}
  Counted(const Counted& o) : v(o.v + 1) {}
};
// Not trivial for calls: passed by address.
struct Indirect {
  Indirect(int x) : c(x), pad(x) {}
  Indirect(const Indirect&) = default;
  Counted c;
  int pad;
};

// Trivial for calls, though its copy is called for its exception specification.
struct MayThrow {
  MayThrow() = default;
  MayThrow(const MayThrow&) noexcept(false) = default;
  long a;
};

// [class.copy.ctor]: a volatile scalar does not make the copy nontrivial.
struct Volatile {
  Volatile(int v) : x(v) {}
  Volatile(const Volatile&) = default;
  volatile int x;
};

struct Aggregate { int x; char y; };
struct VolatileClass { VolatileClass() = default; volatile Aggregate p; char c; };

struct CopyOnly { CopyOnly(const CopyOnly&) = default; int x; char c; };
struct DefaultAndCopy { DefaultAndCopy() = default; DefaultAndCopy(const DefaultAndCopy&) = default; int x; char c; };
struct DefaultAndOther { DefaultAndOther() = default; DefaultAndOther(int); int x; char c; };
struct MoveOnly { MoveOnly(MoveOnly&&) = default; int x; char c; };
struct DeletedCopy { DeletedCopy() = default; DeletedCopy(const DeletedCopy&) = delete; int x; char c; };
struct Assigned { Assigned& operator=(Assigned&&) = default; int x; char c; };
class Private { public: Private() = default; Private(const Private&) = default; private: int x; char c; };
struct VolatileMember { VolatileMember() = default; VolatileMember(const VolatileMember&) = default; volatile int x; char c; };
struct DeletedVolatile { DeletedVolatile() = default; DeletedVolatile(const DeletedVolatile&) = delete; volatile Aggregate p; char c; };
struct Initialized { Initialized() = default; int x = 1; char c; };
struct Member { Member() = default; DefaultAndOther m; char c; };
template <class T> struct Templated { Templated(const Templated&) = default; ~Templated() = default; T x; char c; };

template <class T> struct After : T { char last; };

constexpr int layouts = 13;
static int layout(int i) {
  const int sizes[layouts] = {
      sizeof(After<CopyOnly>), sizeof(After<DefaultAndCopy>), sizeof(After<DefaultAndOther>), sizeof(After<MoveOnly>),
      sizeof(After<DeletedCopy>), sizeof(After<Assigned>), sizeof(After<Private>), sizeof(After<Initialized>),
      sizeof(After<Member>), sizeof(After<Templated<int>>),
      sizeof(After<VolatileMember>), sizeof(After<VolatileClass>), sizeof(After<DeletedVolatile>)};
  return sizes[i];
}

int peer_layout(int i);
Triple peer_swap(Triple t);
Indirect peer_next(Indirect i);
int peer_sum(Indirect i);
long peer_first(MayThrow m, long b);
Volatile peer_volatile(Volatile v);
