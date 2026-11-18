// ζ s3 (#88): a partial specialization's non-type argument whose parameter's type depends on a
// parameter of the partial specialization ([temp.spec.partial.general]/9.2).
template<class T, T t> struct C { };
template<class T> struct C<T, 1> { }; //! error
int main() { return 0; }
