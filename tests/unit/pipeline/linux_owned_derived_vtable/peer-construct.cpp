#include "base.hpp"

int main() {
    D first;
    D second;
    first.field = 73;
    second.field = -19;
    if (dispatch(first) != 73) return 21;
    if (dispatch(second) != -19) return 22;
    if (const int result = check_rtti(first, first)) return result;
    return check_rtti(second, second);
}
