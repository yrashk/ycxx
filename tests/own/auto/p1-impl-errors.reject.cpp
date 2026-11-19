// θ step 1 (#279): placeholder errors not covered elsewhere: `auto` in a typedef
// ([dcl.spec.auto.general]/6), a trailing return type without `auto` alone ([dcl.fct]/2), `auto`
// deduced from a braced list in a return ([dcl.type.auto.deduct]/2), and an `auto&` return with no
// return statement (deduced from `void`).
typedef auto T; //! error
int g() -> int; //! error
auto h() { return {1}; } //! error
int x;
auto& k() { } //! error
int main() { return 0; }
