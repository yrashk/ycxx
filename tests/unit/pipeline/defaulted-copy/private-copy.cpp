#include "private-copy.hpp"

struct Copier {
    static int read(const PrivateCopy& source) {
        PrivateCopy copy(source);
        return copy.value;
    }
};

int main() {
    const PrivateCopy source(37);
    return Copier::read(source) == 37 ? 0 : 1;
}
