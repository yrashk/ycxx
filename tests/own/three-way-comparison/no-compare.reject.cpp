// [expr.spaceship], [cmp.categories.pre]: the built-in operator's result type is a class of
// <compare>, which must be declared before it is used.
int f(int a, int b) {
    return (a <=> b) < 0;  //! error
}
