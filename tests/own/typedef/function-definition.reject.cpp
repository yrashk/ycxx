// #133, from the step's review: a function definition may not be declared typedef
// ([dcl.typedef]/1).
typedef int f(int) { return 0; } //! error
