#include "move-assignment.hpp"

int main(int argc, char**) {
    const MoveAssigned value = MoveAssigned(argc);
    return value.value == argc ? 0 : 1;
}
