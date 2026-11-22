// [basic.lookup.unqual] searches the enumeration scope before the enclosing class.
struct Owner {
    static constexpr long long member = 90;
    enum class Result : int {
        member = 7,
        next = member + 1,
        width = sizeof(member),
    };
};

static_assert(static_cast<int>(Owner::Result::next) == 8);
static_assert(static_cast<int>(Owner::Result::width) == sizeof(int));
