// #1417 [class.copy.ctor], [special], [class.prop], [over.match.viable]: an optional-like copy
// constructor family: a constrained `= default` one for trivially copyable payloads, a
// constrained user-provided one for the rest. The eligible one is the one called and decides
// whether the class is trivially copyable.
int copies = 0;

struct Logged {
  int v;
  Logged(int x) : v(x) {}
  Logged(const Logged& o) : v(o.v + 1) { ++copies; }
};

template<class T> concept TriviallyCopyable = __is_trivially_copyable(T);

template<class T>
struct Opt {
  T payload;
  bool user_copied = false;
  Opt(T p) : payload(p) {}
  Opt(const Opt&) requires TriviallyCopyable<T> = default;
  Opt(const Opt& o) requires (!TriviallyCopyable<T>) : payload(o.payload), user_copied(true) {}
};

static_assert(__is_trivially_copyable(Opt<int>));
static_assert(__is_trivially_constructible(Opt<int>, const Opt<int>&));
static_assert(!__is_trivially_copyable(Opt<Logged>));
static_assert(!__is_trivially_constructible(Opt<Logged>, const Opt<Logged>&));

int main() {
  Opt<int> a(5);
  Opt<int> b(a);
  if (b.payload != 5 || b.user_copied) return 1;

  Opt<Logged> c(Logged(10));
  copies = 0;
  Opt<Logged> d(c);
  if (!d.user_copied || d.payload.v != c.payload.v + 1 || copies != 1) return 2;
  return 0;
}
