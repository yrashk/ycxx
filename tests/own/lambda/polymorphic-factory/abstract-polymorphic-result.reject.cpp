// [class.abstract], [expr.type.conv]: a factory cannot create an abstract result object.
// XFAIL: ycxx #309 diagnoses the pure virtual declaration before checking the return
struct Curtain {
    explicit Curtain(unsigned);
    virtual int width() const = 0;
    virtual ~Curtain();
};

template<class Factory>
int schedule_factory(Factory&&) { return 24; }

template<class Result>
int register_width(unsigned slot) {
    return schedule_factory([slot] { return Result(slot); }); //! error
}

int entry(unsigned slot) { return register_width<Curtain>(slot); }
