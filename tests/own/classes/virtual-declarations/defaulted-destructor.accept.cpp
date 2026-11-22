struct Defaulted {
    virtual ~Defaulted() = default;
};

Defaulted* pass_pointer(Defaulted* value) {
    return value;
}

Defaulted pass_by_value(Defaulted);

int main() {
    return pass_pointer(nullptr) != nullptr;
}
