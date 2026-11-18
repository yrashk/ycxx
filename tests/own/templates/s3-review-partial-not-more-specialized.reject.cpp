// ζ s3 (#88), from the step's review: a partial specialization no more specialized than its
// primary template ([temp.spec.partial.general]: it shall be more specialized, by
// [temp.spec.partial.order]; no "no diagnostic required").
// XFAIL: gcc GCC 16 accepts a partial specialization that only permutes the primary's parameters
template<class T, class U> struct P { };
template<class T, class U> struct P<U, T> { }; //! error
int main() { return 0; }
