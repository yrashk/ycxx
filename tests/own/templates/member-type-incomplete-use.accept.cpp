// [temp.inst]/2. Naming the pointer requires no definition of Wrap<-2>.
template<int N> struct Buffer { int data[N]; };
template<int N> struct Wrap { Buffer<N + 1> storage; };

Wrap<-2>* observe();
