// #1417 [class.copy.assign], [special], [class.prop], [over.match.viable]: copy and move
// assignment operator families with a constrained `= default`, a constrained user-provided
// and a constrained `= delete` member; the eligible ones are called and decide triviality.
// XFAIL: ycxx a deleted move assignment in a class template is not supported yet (#157), and type-trait built-ins do not classify class template specializations yet (#218)
int user_assigns = 0;

struct Heavy {
  int v;
  Heavy& operator=(const Heavy& o) { v = o.v; return *this; }
};

template<class T> concept Trivial = __is_trivially_copyable(T);
template<class T> concept Assignable = requires(T& a, const T& b) { a = b; };
template<class T> concept MoveAssignable = requires(T& a, T&& b) { a = static_cast<T&&>(b); };

template<class T>
struct Opt {
  T payload;
  Opt& operator=(const Opt&) requires Trivial<T> = default;
  Opt& operator=(const Opt& o) requires (!Trivial<T>) {
    payload = o.payload;
    ++user_assigns;
    return *this;
  }
  Opt& operator=(Opt&&) requires Trivial<T> = default;
  Opt& operator=(Opt&&) requires (!Trivial<T>) = delete;
};

static_assert(__is_trivially_assignable(Opt<int>&, const Opt<int>&));
static_assert(__is_trivially_assignable(Opt<int>&, Opt<int>&&));
static_assert(__is_trivially_copyable(Opt<int>));
static_assert(!__is_trivially_assignable(Opt<Heavy>&, const Opt<Heavy>&));
static_assert(!__is_trivially_copyable(Opt<Heavy>));
static_assert(Assignable<Opt<Heavy>> && !MoveAssignable<Opt<Heavy>>);
static_assert(Assignable<Opt<int>> && MoveAssignable<Opt<int>>);

int main() {
  Opt<int> a{1}, b{2};
  a = b;
  if (a.payload != 2 || user_assigns != 0) return 1;
  a = static_cast<Opt<int>&&>(b);
  if (user_assigns != 0) return 2;

  Opt<Heavy> c{{3}}, d{{4}};
  c = d;
  if (c.payload.v != 4 || user_assigns != 1) return 3;
  return 0;
}
