// [except.spec], [expr.unary.noexcept], [temp.inst], [temp.variadic]
struct Callback {
    int operator()() const noexcept;
    int operator()(int) const;
    int operator()(int, int) const noexcept;
    int operator()(int, double) const noexcept(false);
};

template<class F> struct Scope {
    F& callback;

    template<class... Args>
    int run(Args... args) noexcept(noexcept(callback(args...))) {
        return callback(args...);
    }
};

extern Scope<Callback>& scope;

static_assert(noexcept(scope.run()));
static_assert(!noexcept(scope.run(1)));
static_assert(noexcept(scope.run(1, 2)));
static_assert(!noexcept(scope.run(1, 2.0)));

int call_each() {
    return scope.run() + scope.run(1) + scope.run(1, 2) + scope.run(1, 2.0);
}
