// [stmt.if], [stmt.return], [dcl.init.general]
struct Heading {
    explicit Heading(int);
    Heading(const Heading&) = delete;
    virtual ~Heading();
};

template<class Recipe>
constexpr int register_recipe(Recipe&&) { return 41; }

template<class Result>
constexpr int plan(int index) {
    if consteval {
        return register_recipe([index] { return Result(index + 2); });
    } else {
        return index - 1;
    }
}

int runtime_plan(int index) { return plan<Heading>(index); }
