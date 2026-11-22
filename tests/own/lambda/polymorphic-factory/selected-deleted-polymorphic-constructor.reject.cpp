// [stmt.if], [expr.type.conv]: the consteval arm is still checked at a runtime use.
struct Plaque {
    explicit Plaque(long);
    explicit Plaque(int) = delete;
    virtual ~Plaque();
};

template<class Factory>
constexpr int describe(Factory&&) { return 7; }

template<class Result>
constexpr int assign_number(int number) {
    if consteval {
        return describe([number] { return Result(number); }); //! error
    } else {
        return number + 3;
    }
}

int runtime_number(int number) { return assign_number<Plaque>(number); }
