struct E {};

static_assert(sizeof(E) == 1);
static_assert(alignof(E) == 1);

int return_evaluations = 0;

E make_empty() {
    ++return_evaluations;
    return E{};
}

int read_after(E, int value) {
    return value;
}

int main() {
    const int value = read_after(make_empty(), 17);
    return value == 17 && return_evaluations == 1 ? 0 : 1;
}
