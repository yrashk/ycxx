#ifndef YCXX_EXTERNAL_DATA_414_SHARED_DATA_H
#define YCXX_EXTERNAL_DATA_414_SHARED_DATA_H

namespace data {
struct Record {
    int prefix;
    int value;
};

extern int scalar;
extern int numbers[4];
extern Record record;

int* scalar_address();
int* element_address();
int* member_address();
int values_match(int expected_scalar, int expected_element, int expected_member);
void write_values(int new_scalar, int new_element, int new_member);
}

#endif
