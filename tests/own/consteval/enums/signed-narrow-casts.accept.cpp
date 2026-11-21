enum class Step : signed char { back = -7, forward = 12 };

constexpr Step unnamed = static_cast<Step>(-19);

static_assert(unnamed != Step::back);
static_assert(static_cast<signed char>(unnamed) == -19);
static_assert(static_cast<int>(unnamed) == -19);
static_assert(static_cast<int>(Step::back) == -7);

int main() {
    Step result = unnamed;
    return static_cast<int>(result) == -19 ? 0 : 1;
}
