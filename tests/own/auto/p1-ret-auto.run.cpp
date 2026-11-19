// θ step 1 (#279), spec-only: a function with an `auto` return type deduces it from its return statement,
// decaying and dropping top-level cv; it is usable after its definition
// ([dcl.spec.auto.general], [dcl.type.auto.deduct], [stmt.return]).
template<class A, class B> struct same { static constexpr bool value = false; };
template<class A> struct same<A, A> { static constexpr bool value = true; };

auto answer() { return 42; }
auto letter() { return 'q'; }
const int k = 3;
auto copy_k() { return k; }
int arr[2] = {5, 6};
auto first() { return arr; }

static_assert(same<decltype(answer()), int>::value);
static_assert(same<decltype(letter()), char>::value);
static_assert(same<decltype(copy_k()), int>::value);
static_assert(same<decltype(first()), int*>::value);
static_assert(same<decltype(answer), int()>::value);

int main() {
  if (answer() != 42) return 1;
  if (letter() != 'q') return 2;
  if (copy_k() != 3) return 3;
  if (first()[1] != 6) return 4;
  return 0;
}
