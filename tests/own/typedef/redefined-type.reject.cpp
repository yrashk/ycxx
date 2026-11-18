// #133: a typedef-name redeclared in its scope as another type ([dcl.typedef]/5).
typedef int A;
typedef long A; //! error
