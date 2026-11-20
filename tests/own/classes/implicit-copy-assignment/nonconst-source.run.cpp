#include "nonconst-member.hpp"

int main() {
    Whole source{{37}};
    Whole destination{{7}};
    Whole& assigned = (destination = source);
    return &assigned == &destination && destination.member.value == 38 &&
                   source.member.value == 39
               ? 0
               : 1;
}
