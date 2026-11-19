// ζ p4 (#88): a function template's pack is followed only by parameters that are deduced from
// its parameter-type-list or have a default ([temp.param]; the draft's example in
// [temp.param] says so too).
// XFAIL: clang accepts the declarations and fails only at a call (as on the draft's example, tests/draft/DISAGREEMENTS.md)
// XFAIL: gcc accepts the declarations and fails only at a call (as on the draft's example, tests/draft/DISAGREEMENTS.md)
template<class... T, class... U> void f() { } //! error
template<class... T, class U> void g() { } //! error
template<class... T, class U> void k(U) { }
template<class... T, class U = int> void m() { }
int main() { return 0; }
