// #297: [dcl.type.pack.index]/1: the typedef-name denotes a pack.
template<class T> using P = T...[0]; //! error
int main() { return 0; }
