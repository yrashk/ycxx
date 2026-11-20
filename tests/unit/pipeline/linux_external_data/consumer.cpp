#include "shared_data.h"

static int* volatile scalar_pointer = &data::scalar;
static int* volatile element_by_addition = 2 + data::numbers;
static int* volatile element_by_subscript = &data::numbers[2];
static int* volatile member_pointer = &data::record.value;

int main() {
    if (scalar_pointer != &data::scalar || scalar_pointer != data::scalar_address()) return 1;
    if (element_by_addition != &data::numbers[2] || element_by_subscript != data::element_address() ||
        element_by_addition != element_by_subscript || element_by_addition == &data::numbers[0]) {
        return 2;
    }
    if (member_pointer != &data::record.value || member_pointer != data::member_address() ||
        member_pointer == &data::record.prefix) {
        return 3;
    }

    if (data::scalar != 17 || data::numbers[2] != 7 || data::record.value != 19 ||
        !data::values_match(17, 7, 19)) {
        return 4;
    }

    data::scalar = 23;
    data::numbers[2] = 29;
    data::record.value = 31;
    if (*scalar_pointer != 23 || *element_by_addition != 29 || *member_pointer != 31 ||
        !data::values_match(23, 29, 31)) {
        return 5;
    }

    *scalar_pointer = 37;
    *element_by_subscript = 41;
    *member_pointer = 43;
    if (!data::values_match(37, 41, 43)) return 6;

    data::write_values(47, 53, 59);
    if (data::scalar != 47 || data::numbers[2] != 53 || data::record.value != 59) return 7;
    if (data::numbers[0] != 3 || data::numbers[1] != 5 || data::numbers[3] != 11 ||
        data::record.prefix != 13) {
        return 8;
    }
    return 0;
}
