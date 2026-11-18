// ζ s3 (#88): a partial specialization whose arguments are the primary template's own
// parameters specializes nothing ([temp.spec.partial.general]/9.3).
template<class T, int N> struct D { };
template<class U, int M> struct D<U, M> { }; //! error
int main() { return 0; }
