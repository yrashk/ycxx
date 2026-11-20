// #367, spec-only ([dcl.init.ref], [expr.const.init], [basic.start]).
// EXIT: 0
namespace state {
static int target = 19;
static int& bound = target;
static int* volatile observed_address = &bound;
static int observed_value = bound;
}

int main() {
    if (&state::bound != &state::target || state::observed_address != &state::target) return 1;
    if (state::bound != 19 || state::observed_value != 19) return 2;

    state::bound = 23;
    if (state::target != 23) return 3;

    state::target = 29;
    if (state::bound != 29 || &state::bound != state::observed_address) return 4;
    return 0;
}
