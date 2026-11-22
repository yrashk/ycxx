// [dcl.type.decltype] gives the declared type of the member.
struct Owner {
    unsigned char member;
    enum class Result : decltype(member) { value = 7 };
};

static_assert(sizeof(Owner::Result) == sizeof(unsigned char));
static_assert(static_cast<unsigned char>(Owner::Result::value) == 7);
