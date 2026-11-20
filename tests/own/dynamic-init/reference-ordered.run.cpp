// #367, spec-only ([dcl.init.ref], [expr.const.init], [basic.start]).
// EXIT: 0
namespace state {
static int first = 31;
static int second = 41;
static volatile int select_second = 1;
static int initialization_calls = 0;

int& choose_target() {
    ++initialization_calls;
    if (select_second != 0) return second;
    return first;
}

static int& bound = choose_target();
static int* observed_address = &bound;
static int observed_value = bound;
static int observed_call_count = initialization_calls;
}

int main() {
    if (state::initialization_calls != 1 || state::observed_call_count != 1) return 1;
    if (&state::bound != &state::second || state::observed_address != &state::second) return 2;
    if (state::bound != 41 || state::observed_value != 41) return 3;

    state::select_second = 0;
    state::second = 53;
    if (&state::bound != &state::second || state::bound != 53) return 4;

    state::bound = 61;
    if (state::second != 61 || state::first != 31) return 5;
    if (state::initialization_calls != 1 || state::observed_value != 41) return 6;
    return 0;
}
