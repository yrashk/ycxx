// [temp.inst]/2. [dcl.array]/1. A complete Wrap<-2> needs Buffer<-1>.
template<int N> struct Buffer { int data[N]; }; //! error
template<int N> struct Wrap { Buffer<N + 1> storage; };

static_assert(sizeof(Wrap<-2>) > 0);
