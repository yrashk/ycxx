struct Root {
    virtual int value() const { return 17; }
};

int dispatch(const Root& object) {
    return object.value();
}

int main() {
    return 0;
}
