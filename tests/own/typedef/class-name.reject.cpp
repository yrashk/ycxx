// #133: a typedef-name may redeclare a class of its scope only as that class ([dcl.typedef]/5).
struct S {};
typedef int S; //! error
