// #133, from the step's review: a class may not take the name of a typedef-name of another
// type in its scope ([basic.scope.scope]/4).
typedef int S;
struct S {}; //! error
