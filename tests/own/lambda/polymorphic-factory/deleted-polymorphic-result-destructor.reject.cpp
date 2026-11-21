// [stmt.return]: the selected destructor must be usable for the lambda's return value.
struct Marker {
    explicit Marker(unsigned);
    virtual ~Marker() = delete;
};

template<class Factory>
int label_factory(Factory&&) { return 33; }

template<class Result>
int label_marker(unsigned code) {
    return label_factory([code] { return Result(code); }); //! error
}

int entry(unsigned code) { return label_marker<Marker>(code); }
