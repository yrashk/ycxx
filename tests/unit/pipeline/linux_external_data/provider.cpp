#include "shared_data.h"

namespace data {
int scalar = 17;
int numbers[4] = {3, 5, 7, 11};
Record record = {13, 19};

int* scalar_address() { return &scalar; }
int* element_address() { return numbers + 2; }
int* member_address() { return &record.value; }

int values_match(int expected_scalar, int expected_element, int expected_member) {
    return scalar == expected_scalar && numbers[2] == expected_element &&
           record.value == expected_member;
}

void write_values(int new_scalar, int new_element, int new_member) {
    scalar = new_scalar;
    numbers[2] = new_element;
    record.value = new_member;
}
}
