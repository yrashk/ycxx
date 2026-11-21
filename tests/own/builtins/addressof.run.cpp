// [specialized.addressof] specifies the actual address even with overloaded &.
struct Overloaded {
  int value;

  int* operator&() { return nullptr; }
};

int global = 42;
constexpr int* global_address = __builtin_addressof(global);
static_assert(global_address == &global);

template<class A, class B>
constexpr bool same_type = false;

template<class A>
constexpr bool same_type<A, A> = true;

template<class T>
constexpr T* actual_address(T& value) {
  return __builtin_addressof(value);
}

int main() {
  Overloaded local{42};
  const volatile int qualified = 7;
  static_assert(same_type<decltype(__builtin_addressof(qualified)), const volatile int*>);
  if (&local != nullptr || __builtin_addressof(local)->value != 42)
    return 1;
  if (__builtin_addressof(qualified) != &qualified)
    return 2;
  if (actual_address(local) != __builtin_addressof(local))
    return 3;
  return 0;
}
