#define LOCAL_CTOR_OFFSET 10
#include "constructors.hpp"

#if defined(DUPLICATE_CONSTRUCTOR)
OutOfLine::OutOfLine(int input) : value(input + 11) {}
#endif

bool check_peer(const InClass& caller) {
    int input = caller.value + 1;
    InClass in_class(input);
    OnDefinition on_definition(input);
    OnDeclaration on_declaration(input);
    LocalCtor local(input);
    OutOfLine out_of_line(input);
    return in_class.value == 8 && on_definition.value == 10 && on_declaration.value == 11 &&
           local.value == 18 && out_of_line.value == 19;
}
