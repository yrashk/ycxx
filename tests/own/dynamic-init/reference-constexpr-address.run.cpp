// #367, spec-only ([dcl.init.ref], [expr.const.init], [basic.start]).
// EXIT: 0
static int target = 53;
static constexpr int& bound = target;
static constexpr int* address = &bound;
static_assert(address == &target);

int main() {
    bound = 59;
    return target == 59 && *address == 59 ? 0 : 1;
}
