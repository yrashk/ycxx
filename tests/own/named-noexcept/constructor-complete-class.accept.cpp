constexpr bool promise = false;

struct LaterMember {
    LaterMember() noexcept(promise);
    static constexpr bool promise = true;
};

LaterMember::LaterMember() noexcept {}

struct CompleteSize {
    CompleteSize() noexcept(sizeof(CompleteSize) != 0);
};

CompleteSize::CompleteSize() noexcept {}
