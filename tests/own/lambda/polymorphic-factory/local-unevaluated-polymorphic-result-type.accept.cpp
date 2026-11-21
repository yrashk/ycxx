struct Proof {
    explicit Proof(int);
    Proof(const Proof&) = delete;
    Proof(Proof&&) = delete;
    virtual ~Proof();
};

void check_result_type() {
    auto factory = [] { return Proof(1); };
    decltype(factory())* result = nullptr;
    Proof* checked = result;
    result = checked;
}
