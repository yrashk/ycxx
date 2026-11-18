// ζ s3 (#88): an explicit specialization names a template ([temp.expl.spec]/1); a function whose
// type no template of the name has ([temp.deduct.decl]).
template<> struct Missing<int> { }; //! error
template<class T> T one(T x) { return x; }
template<> long one(int x) { return x; } //! error
int main() { return 0; }
