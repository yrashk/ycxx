template<class T>
concept SignedInt = __is_same(T, int);

template<SignedInt T>
constexpr int token = 19;

static_assert(__is_same(decltype(token<int>), const int));
constexpr const int* address = &token<int>;
static_assert(*address == 19);

int main() {
    const int* pointer = &token<int>;
    return *pointer == 19 ? 0 : 1;
}
