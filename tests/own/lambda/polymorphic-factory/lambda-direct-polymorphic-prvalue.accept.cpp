// [stmt.return], [dcl.init.general]: the deduced result is initialized directly from the prvalue.
struct Envelope {
    explicit Envelope(unsigned);
    Envelope(const Envelope&) = delete;
    Envelope(Envelope&&) = delete;
    virtual ~Envelope();
};

void keep_recipe(unsigned number) {
    auto recipe = [number] { return Envelope(number); };
    (void)recipe;
}
