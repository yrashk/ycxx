// ζ m9 (#88, #314), from the step's review: static member templates of a class template, called through an object, the class, and
// with explicit arguments ([temp.mem], [class.static.mfct]).
// EXIT: 0
template<class V> struct W { template<class T> static int mk(T) { return 4; } template<class T> static V cv(T t) { return V(t); } };
int main() { W<int> w; return w.mk(1) + W<int>::mk(3) + w.template mk<long>(2) + W<long>::cv(5) - 17; }
