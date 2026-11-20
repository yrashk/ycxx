#include "shared_data.h"

static int* volatile initialized_pointer = &data::initialized;
static int* volatile zero_pointer = &data::zero_initialized;
static int* volatile redeclared_pointer = &data::redeclared;
static int* volatile specialized_pointer = &data::specialized<int>;
static const int* volatile immutable_pointer = &data::immutable;
static const int* volatile named_immutable = &data::immutable;
static int* const named[] = {&data::initialized, &data::zero_initialized,
                             &data::redeclared, &data::specialized<int>};

int* main_initialized_address() { return initialized_pointer; }
int* main_zero_address() { return zero_pointer; }
int* main_redeclared_address() { return redeclared_pointer; }
int* main_specialized_address() { return specialized_pointer; }
const int* main_immutable_address() { return immutable_pointer; }

int main() {
    const int* local_immutable = main_immutable_address();
    const int* peer_immutable = peer_immutable_address();
    if (local_immutable != named_immutable || peer_immutable != local_immutable ||
        *local_immutable != 37) {
        return 1;
    }

    int* const local[] = {main_initialized_address(), main_zero_address(),
                          main_redeclared_address(), main_specialized_address()};
    int* const peer[] = {peer_initialized_address(), peer_zero_address(),
                         peer_redeclared_address(), peer_specialized_address()};
    const int initial[] = {17, 0, 23, 31};

    for (int index = 0; index < 4; ++index) {
        if (local[index] != named[index] || local[index] != peer[index]) return 2 + index;
        if (*local[index] != initial[index]) return 6 + index;
    }

    *initialized_pointer = 41;
    *zero_pointer = 43;
    *redeclared_pointer = 47;
    *specialized_pointer = 53;
    if (!peer_values_match(41, 43, 47, 53)) return 10;

    peer_write_values(59, 61, 67, 71);
    if (*initialized_pointer != 59 || *zero_pointer != 61 || *redeclared_pointer != 67 ||
        *specialized_pointer != 71) {
        return 11;
    }
    return 0;
}
