#include "scalar-copy.hpp"

Scalar::Scalar(int input) : value(input) {}

int main(int argc, char**) {
    const int input = argc % 7;
    const Scalar source(input);
    Scalar copy(source);
    if (copy.value != input || &copy.value == &source.value) {
        return 1;
    }

    copy.value += 3;
    return source.value == input && copy.value == input + 3 ? 0 : 2;
}
