// #367, spec-only ([dcl.init.ref], [expr.const.init], [basic.start]).
// EXIT: 0
static int target = 37;
static constinit int& bound = target;

int main() {
    if (&bound != &target || bound != 37) return 1;
    bound = 43;
    return target == 43 ? 0 : 2;
}
