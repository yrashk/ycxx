#include "shared_data.h"

static int* volatile initialized_pointer = &data::initialized;
static int* volatile zero_pointer = &data::zero_initialized;
static int* volatile redeclared_pointer = &data::redeclared;
static int* volatile specialized_pointer = &data::specialized<int>;
static const int* volatile immutable_pointer = &data::immutable;

const int* peer_seed_address() { return &Record::seed; }
const Descriptor* peer_descriptor_address() { return &Record::descriptor; }
const int* peer_local_seed_address() { return &LocalRecord::seed; }
int* peer_initialized_address() { return initialized_pointer; }
int* peer_zero_address() { return zero_pointer; }
int* peer_redeclared_address() { return redeclared_pointer; }
int* peer_specialized_address() { return specialized_pointer; }
const int* peer_immutable_address() { return immutable_pointer; }

int peer_values_match(int initialized, int zero, int redeclared, int specialized) {
    return *initialized_pointer == initialized && *zero_pointer == zero &&
           *redeclared_pointer == redeclared && *specialized_pointer == specialized;
}

void peer_write_values(int initialized, int zero, int redeclared, int specialized) {
    *initialized_pointer = initialized;
    *zero_pointer = zero;
    *redeclared_pointer = redeclared;
    *specialized_pointer = specialized;
}
