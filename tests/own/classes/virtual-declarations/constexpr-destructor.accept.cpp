struct ExceptionShape {
    constexpr virtual ~ExceptionShape() {}

    virtual const char* what() const noexcept {
        return "exception";
    }
};

const ExceptionShape* pass_pointer(const ExceptionShape* value) {
    return value;
}

const ExceptionShape& pass_reference(const ExceptionShape& value) {
    return value;
}

ExceptionShape pass_by_value(ExceptionShape);

int main() {
    return pass_pointer(nullptr) != nullptr;
}
