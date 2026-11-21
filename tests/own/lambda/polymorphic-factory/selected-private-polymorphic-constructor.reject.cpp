// [stmt.if], [expr.type.conv]: the else arm selects the exact private constructor.
class Tablet {
public:
    explicit Tablet(long);
    virtual ~Tablet();

private:
    explicit Tablet(int);
};

template<class Factory>
constexpr int record_factory(Factory&&) { return 18; }

template<class Result>
constexpr int record(int serial) {
    if consteval {
        return serial + 1;
    } else {
        return record_factory([serial] { return Result(serial); }); //! error
    }
}

int runtime_record(int serial) { return record<Tablet>(serial); }
