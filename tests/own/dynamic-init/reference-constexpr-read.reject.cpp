// #367, spec-only ([dcl.init.ref], [expr.const.init], [basic.start]).
int target = 53;
constexpr int& bound = target;
constexpr int copied_value = bound; //! error
