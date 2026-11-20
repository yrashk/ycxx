#include <cstdarg>

extern "C" int forward_to_host(int expected, ...);

extern "C" int host_read_double(std::va_list cursor, int expected) {
    return va_arg(cursor, double) == expected + 0.25 ? 0 : 1;
}

int main() {
    if (forward_to_host(3, 3.25) != 0) {
        return 1;
    }
    return forward_to_host(11, 11.25) == 0 ? 0 : 2;
}
