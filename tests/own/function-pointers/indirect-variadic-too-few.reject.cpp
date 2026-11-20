// [expr.call]
using Variadic = int (*)(int, ...);

int missing_named_argument(Variadic target) {
    return target();  //! error
}
