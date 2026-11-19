// ζ m9 (#88, #314): a member template defined outside its class matches a member template the
// class declares ([temp.mem]/1, [class.mfct]): S has no `get` of this signature.
struct S { template<class T> T get(T t) const; };
template<class T> T S::get(T t) { return t; } //! error
int main() { return 0; }
