[[gnu::visibility("hidden")]] int hidden_from_decl(int);

int hidden_from_decl(int value) {
    return value + 1;
}

int hidden_from_def(int);

[[gnu::visibility("hidden")]] int hidden_from_def(int value) {
    return value + 2;
}

[[gnu::visibility("hidden")]] int strong_hidden(int value) {
    return value + 3;
}

inline int inline_default(int value) {
    return value + 4;
}

static int static_local(int value) {
    return value + 5;
}

using Unary = int (*)(int);

Unary default_addr() {
    return &inline_default;
}

Unary local_addr() {
    return &static_local;
}

[[gnu::visibility("hidden")]] int overload(int value) {
    return value + 6;
}

int overload(long value) {
    return value + 7;
}

[[deprecated("hidden")]] int unrelated_attr(int value) {
    return value + 8;
}
