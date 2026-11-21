#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <new>

struct Token;

extern "C" void release_int_array(int*);
extern "C" void release_pointer_array(Token**);
extern "C" void release_observed_array(int*);

namespace {
struct Observation {
    std::uintptr_t allocated;
    std::uintptr_t released;
    std::uintptr_t operand;
    std::size_t allocated_bytes;
    std::size_t sized_bytes;
    unsigned new_calls;
    unsigned unsized_calls;
    unsigned sized_calls;
    unsigned operand_calls;
};

Observation observed{};
bool tracking = false;
void* outstanding = nullptr;

void begin_case() {
    observed = {};
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
    void* pointer = std::malloc(bytes == 0 ? 1 : bytes);
    if (!pointer)
        throw std::bad_alloc{};

    if (tracking) {
        if (outstanding)
            std::abort();
        outstanding = pointer;
        observed.allocated = reinterpret_cast<std::uintptr_t>(pointer);
        observed.allocated_bytes = bytes;
        ++observed.new_calls;
    }
    return pointer;
}

void operator delete[](void* pointer) noexcept {
    if (tracking) {
        observed.released = reinterpret_cast<std::uintptr_t>(pointer);
        ++observed.unsized_calls;
    }
    release_storage(pointer);
}

void operator delete[](void* pointer, std::size_t bytes) noexcept {
    if (tracking) {
        observed.released = reinterpret_cast<std::uintptr_t>(pointer);
        observed.sized_bytes = bytes;
        ++observed.sized_calls;
    }
    release_storage(pointer);
}

extern "C" int* observe_array_operand(int* pointer) {
    observed.operand = reinterpret_cast<std::uintptr_t>(pointer);
    ++observed.operand_calls;
    return pointer;
}

namespace {
template<class Element>
bool array_case(const char* name, std::size_t count, void (*release)(Element*),
                unsigned expected_operand_calls = 0) {
    begin_case();
    Element* array = new Element[count];
    const auto input_address = reinterpret_cast<std::uintptr_t>(array);
    release(array);

    const bool exact_address = observed.allocated == input_address &&
                               observed.released == input_address;
    const bool exact_operand = observed.operand_calls == expected_operand_calls &&
                               (expected_operand_calls == 0 || observed.operand == input_address);
    const bool valid = observed.new_calls == 1 && input_address != 0 &&
                       observed.allocated_bytes == count * sizeof(Element) &&
                       exact_address && exact_operand && outstanding == nullptr &&
                       observed.unsized_calls == 1 && observed.sized_calls == 0;

    std::printf("%s new=%u bytes=%zu unsized=%u sized=%u sized-bytes=%zu operand=%u exact-address=%d status=%s\n",
                name, observed.new_calls, observed.allocated_bytes, observed.unsized_calls,
                observed.sized_calls, observed.sized_bytes, observed.operand_calls,
                exact_address, valid ? "pass" : "fail");
    end_case();
    return valid;
}

template<class Element>
bool null_case(const char* name, void (*release)(Element*), bool require_skip,
               unsigned expected_operand_calls = 0) {
    begin_case();
    release(nullptr);

    const bool valid = observed.new_calls == 0 &&
                       observed.operand_calls == expected_operand_calls &&
                       observed.operand == 0 && observed.released == 0 &&
                       (!require_skip || (observed.unsized_calls == 0 && observed.sized_calls == 0));

    std::printf("%s unsized=%u sized=%u operand=%u status=%s\n",
                name, observed.unsized_calls, observed.sized_calls, observed.operand_calls,
                valid ? "pass" : "fail");
    end_case();
    return valid;
}
}

int main(int argc, char** argv) {
    const bool require_skip = argc == 2 && std::strcmp(argv[1], "--require-null-skip") == 0;
    if (argc != 1 && !require_skip)
        return 2;

    bool valid = array_case<int>("int[3]", 3, release_int_array);
    valid &= array_case<Token*>("Token*[2]", 2, release_pointer_array);
    valid &= array_case<int>("int[0]", 0, release_int_array);
    valid &= array_case<int>("observed int[4]", 4, release_observed_array, 1);
    valid &= null_case<int>("null int[]", release_int_array, require_skip);
    valid &= null_case<Token*>("null Token*[]", release_pointer_array, require_skip);
    valid &= null_case<int>("observed null int[]", release_observed_array, require_skip, 1);

    return valid ? 0 : 1;
}
