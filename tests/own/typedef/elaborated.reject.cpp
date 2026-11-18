// #133: a typedef-name after a class-key ([dcl.type.elab]/2).
typedef int T;
struct T* p; //! error
