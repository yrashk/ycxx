int trace;

struct Value {
    int payload;
    Value(int input) {
        trace = trace * 10 + 1;
        payload = input;
    }
};

Value& destination(Value& value) {
    trace = trace * 10 + 2;
    return value;
}

int main() {
    Value value(3);
    trace = 0;
    Value& result = (destination(value) = 42);
    return &result == &value && value.payload == 42 && trace == 12 ? 0 : 1;
}
