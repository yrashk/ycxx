// ζ s3 (#88): an explicit specialization declared after a use that instantiated the
// specialization ([temp.expl.spec]/7).
template<class T> T id(T x) { return x; }
int use() { return id(1); }
template<> int id<int>(int x) { return x + 1; } //! error
int main() { return use(); }
