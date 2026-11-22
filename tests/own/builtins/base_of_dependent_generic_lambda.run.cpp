// #218: a dependent `__is_base_of` in a generic lambda outside any template is evaluated for each
// specialization of its call operator, with Derived's completeness at the end of the unit
// ([temp.point]), so classes completed after the lambda count.
// EXIT: 12
struct B {};
struct Late;
auto make() { return [](auto* p) { return __is_base_of(B, __remove_pointer(decltype(p))) ? 1 : 2; }; }
struct Late : B {};
struct U {};
int main() {
  auto l = make();
  return l(static_cast<Late*>(nullptr)) * 10 + l(static_cast<U*>(nullptr));
}
