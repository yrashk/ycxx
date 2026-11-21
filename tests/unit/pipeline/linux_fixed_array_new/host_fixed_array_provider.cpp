#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <new>

struct Token {
    int value;
};

extern "C" int* make_int_array();
extern "C" Token** make_pointer_array();
extern "C" int* make_empty_int_array();
extern "C" void release_int_array(int*);
extern "C" void release_pointer_array(Token**);

namespace {
struct Observation {
    std::uintptr_t allocated;
    std::uintptr_t released;
    std::size_t requested_bytes;
    unsigned requests;
    unsigned unsized_calls;
    unsigned sized_calls;
};

Observation observed{};
void* outstanding = nullptr;
bool tracking = false;
bool fail_allocation = false;

void begin_case(bool fail = false) {
    observed = {};
    fail_allocation = fail;
    tracking = true;
}

void end_case() {
    tracking = false;
    std::free(outstanding);
    outstanding = nullptr;
}

void release_storage(void* pointer) {
    if (!tracking) {
        std::free(pointer);
    } else if (pointer == outstanding) {
        std::free(pointer);
        outstanding = nullptr;
    }
}
}

void* operator new[](std::size_t bytes) {
    if (tracking) {
        ++observed.requests;
        observed.requested_bytes = bytes;
        if (fail_allocation)
            throw std::bad_alloc{};
    }

    void* pointer = std::malloc(bytes == 0 ? 1 : bytes);
    if (!pointer)
        throw std::bad_alloc{};

    if (tracking) {
        if (outstanding)
            std::abort();
        outstanding = pointer;
        observed.allocated = reinterpret_cast<std::uintptr_t>(pointer);
    }
    return pointer;
}

void operator delete[](void* pointer) noexcept {
    if (tracking) {
        ++observed.unsized_calls;
        observed.released = reinterpret_cast<std::uintptr_t>(pointer);
    }
    release_storage(pointer);
}

void operator delete[](void* pointer, std::size_t) noexcept {
    if (tracking) {
        ++observed.sized_calls;
        observed.released = reinterpret_cast<std::uintptr_t>(pointer);
    }
    release_storage(pointer);
}

namespace {
bool check_first_int(int* pointer) {
    volatile int* first = pointer;
    *first = 37;
    return *first == 37;
}

bool check_first_pointer(Token** pointer) {
    static Token token{41};
    Token* volatile* first = pointer;
    *first = &token;
    return *first == &token;
}

template<class Element>
bool array_case(const char* label, std::size_t bytes, Element* (*allocate)(),
                void (*release)(Element*), bool (*check_first)(Element*)) {
    begin_case();
    Element* pointer = allocate();
    const auto returned = reinterpret_cast<std::uintptr_t>(pointer);
    const bool allocation_ok = observed.requests == 1 &&
                               observed.requested_bytes == bytes &&
                               returned != 0 && observed.allocated == returned;
    const bool element_ok = allocation_ok && (!check_first || check_first(pointer));
    if (allocation_ok)
        release(pointer);

    const bool exact_address = allocation_ok && observed.released == returned;
    const bool valid = element_ok && exact_address && outstanding == nullptr &&
                       observed.unsized_calls == 1 && observed.sized_calls == 0;
    const char* readback = check_first ? (element_ok ? "pass" : "fail") : "skip";
    std::printf("%s requests=%u bytes=%zu exact-address=%d first=%s unsized=%u sized=%u status=%s\n",
                label, observed.requests, observed.requested_bytes, exact_address, readback,
                observed.unsized_calls, observed.sized_calls, valid ? "pass" : "fail");
    end_case();
    return valid;
}

bool failure_case() {
    begin_case(true);
    bool caught_bad_alloc = false;
    try {
        static_cast<void>(make_int_array());
    } catch (const std::bad_alloc&) {
        caught_bad_alloc = true;
    } catch (...) {
    }

    const bool valid = caught_bad_alloc && observed.requests == 1 &&
                       observed.requested_bytes == 3 * sizeof(int) &&
                       observed.allocated == 0 && outstanding == nullptr &&
                       observed.unsized_calls == 0 && observed.sized_calls == 0;
    std::printf("failure caught-bad-alloc=%d requests=%u bytes=%zu unsized=%u sized=%u status=%s\n",
                caught_bad_alloc, observed.requests, observed.requested_bytes,
                observed.unsized_calls, observed.sized_calls, valid ? "pass" : "fail");
    end_case();
    return valid;
}
}

int main() {
    bool valid = array_case<int>("int[3]", 3 * sizeof(int),
                                 make_int_array, release_int_array, check_first_int);
    valid &= array_case<Token*>("Token*[2]", 2 * sizeof(Token*),
                               make_pointer_array, release_pointer_array, check_first_pointer);
    valid &= array_case<int>("int[0]", 0, make_empty_int_array, release_int_array, nullptr);
    valid &= failure_case();
    return valid ? EXIT_SUCCESS : EXIT_FAILURE;
}
