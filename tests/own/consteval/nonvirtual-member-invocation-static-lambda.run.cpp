// [expr.const.core], [expr.context]
struct Record {
    static consteval int get() { return 7; }
};

static auto call_with_reference = [](Record& input) {
    Record local{};
    return (false ? input : local).get();
};

int main() {
    Record input{};
    return call_with_reference(input) - 7;
}
