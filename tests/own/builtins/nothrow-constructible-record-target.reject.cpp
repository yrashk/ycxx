// XFAIL: clang,gcc Peers support record construction in this trait
struct Record {
    Record() noexcept;
};
constexpr bool value = __is_nothrow_constructible(Record); //! error
