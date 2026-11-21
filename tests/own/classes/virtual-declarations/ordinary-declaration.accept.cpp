struct Declaration {
    virtual int value() const;
};

Declaration* pass_pointer(Declaration* value) {
    return value;
}

const Declaration& pass_reference(const Declaration& value) {
    return value;
}

Declaration pass_by_value(Declaration);

int main() {
    return pass_pointer(nullptr) != nullptr;
}
