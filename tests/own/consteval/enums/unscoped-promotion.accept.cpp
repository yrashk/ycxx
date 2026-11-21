enum Step : signed char { down = -9, up = 6 };

constexpr int delta = +Step::down;

static_assert(delta == -9);
static_assert(sizeof(+Step::down) == sizeof(int));

int main() {
    return delta == -9 ? 0 : 1;
}
