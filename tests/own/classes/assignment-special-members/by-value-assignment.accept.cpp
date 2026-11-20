struct CopyAssigned {
    int value;

    CopyAssigned(int input) : value(input) {}

    CopyAssigned& operator=(CopyAssigned source) {
        value = source.value + 3;
        return *this;
    }
};

int main() {
    const CopyAssigned source(37);
    CopyAssigned copied(source);
    CopyAssigned destination(11);
    CopyAssigned& result = (destination = source);
    return copied.value == 37 && &result == &destination && destination.value == 40 ? 0 : 1;
}
