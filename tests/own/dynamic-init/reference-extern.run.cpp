// #367, spec-only ([dcl.init.ref], [expr.const.init], [basic.start]).
// EXIT: 0
namespace {
extern int& bound;

int* address_from_declaration() {
    return &bound;
}

int target = 47;
int& bound = target;
}

int main() {
    if (address_from_declaration() != &target || bound != 47) return 1;
    *address_from_declaration() = 49;
    return bound == 49 && target == 49 ? 0 : 2;
}
