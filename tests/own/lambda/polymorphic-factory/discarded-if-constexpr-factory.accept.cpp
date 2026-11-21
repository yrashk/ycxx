// [stmt.if]: the dependent invalid factory is inside the discarded substatement.
struct Plate {
    explicit Plate(long);
    explicit Plate(int) = delete;
    virtual ~Plate();
};

template<class Factory>
constexpr int store_factory(Factory&&) { return 14; }

template<bool UseFactory, class Result>
constexpr int skipped_plan(int index) {
    if constexpr (UseFactory) {
        return store_factory([index] { return Result(index); });
    } else {
        return index + 9;
    }
}

int runtime_plan(int index) { return skipped_plan<false, Plate>(index); }
