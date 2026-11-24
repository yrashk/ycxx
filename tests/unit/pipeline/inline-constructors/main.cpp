#define LOCAL_CTOR_OFFSET 5
#include "constructors.hpp"

OutOfLine::OutOfLine(int input) : value(input + 11) {}

int main() {
    InClass in_class(7);
    OnDefinition on_definition(7);
    OnDeclaration on_declaration(7);
    LocalCtor local(7);
    OutOfLine out_of_line(7);
    if (in_class.value != 7 || on_definition.value != 9 || on_declaration.value != 10) return 1;
    if (local.value != 12 || out_of_line.value != 18) return 2;
    return check_peer(in_class) ? 0 : 3;
}
