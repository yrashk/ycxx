// ζ s3 (#88): two partial specializations match a specialization and neither is more
// specialized than the other ([temp.spec.partial.match]/1.3).
template<class T, class U> struct P { };
template<class T, class U> struct P<T*, U> { };
template<class T, class U> struct P<T, U*> { };
P<int, int> fine;
P<int*, int> first;
P<int*, int*> both; //! error
int main() { return 0; }
