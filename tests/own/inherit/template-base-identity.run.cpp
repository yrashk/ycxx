// Inherited static names designate the base specialization's original objects (#308).
// EXIT: 0
template<bool B> struct Constant { static constexpr bool value = B; };
template<bool B> using Bool = Constant<B>;
template<class T, class U> struct IsSame : Bool<__is_same(T, U)> {};

using Enabled = IsSame<int, int>;
using Disabled = IsSame<int, long>;
const bool& enabled = Enabled::value;
const bool* disabled = &Disabled::value;

int main() {
  return &enabled == &Bool<true>::value && disabled == &Bool<false>::value &&
                 &enabled != disabled ? 0 : 1;
}
