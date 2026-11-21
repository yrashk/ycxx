// [stmt.return]: even an uninvoked factory potentially invokes the result destructor.
class Dial {
public:
    explicit Dial(unsigned);

private:
    virtual ~Dial();
};

template<class Factory>
int count_factory(Factory&&) { return 12; }

template<class Result>
int register_dial(unsigned id) {
    return count_factory([id] { return Result(id); }); //! error
}

int entry(unsigned id) { return register_dial<Dial>(id); }
