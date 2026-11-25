#include "compiler_profile.hh"

namespace compiler = example::compiler;

template<compiler::support State>
constexpr int filesystem_status() {
    if constexpr (compiler::available<State>) {
        return 0;
    } else if constexpr (State == compiler::support::unsupported) {
        return 10;
    } else {
        return 20;
    }
}

int main() {
    return filesystem_status<compiler::filesystem>();
}
