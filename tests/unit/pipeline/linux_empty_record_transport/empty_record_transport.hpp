#ifndef EMPTY_RECORD_TRANSPORT_HPP
#define EMPTY_RECORD_TRANSPORT_HPP

struct E {};

struct Byte {
    unsigned char value;
};

static_assert(sizeof(E) == 1);
static_assert(alignof(E) == 1);
static_assert(sizeof(Byte) == 1);
static_assert(alignof(Byte) == 1);

bool six_integers(E, long a, long b, long c, E, long d, long e, long f, E);
bool stack_integers(E, long a, long b, long c, E, long d, long e, long f, E,
                    long seventh, E, long eighth, E);

E return_empty(E, long a, long b, long c, E, long d, long e, long f, E,
               long seventh, E, long eighth, E);
int return_operand_evaluations();
bool return_arguments_correct();

bool distinct_parameters(E first, E second, const E* original);
bool compare_live_addresses(const E* first, const E* second, const E* original);

Byte transfer_byte(Byte input, long left, long right);

#endif
