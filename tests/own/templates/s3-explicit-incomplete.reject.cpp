// ζ s3 (#88): an explicit specialization only declared is an incomplete class; the template's
// definition is not used for it ([temp.expl.spec]/9).
template<class T> struct X { int v; };
template<> struct X<int>;
X<int>* fine = nullptr;
X<long> also{1};
X<int> x; //! error
int main() { return 0; }
