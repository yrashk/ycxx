struct ScalarRoot {
    virtual int first() const;
    virtual int second() const;

    char letter;
    int word;
    long long wide;
};

struct FieldlessRoot {
    virtual int probe() const;
};

extern "C" int consumer_scalar_size();
extern "C" int consumer_scalar_alignment();
extern "C" char consumer_scalar_letter(const ScalarRoot* value);
extern "C" int consumer_scalar_word(const ScalarRoot& value);
extern "C" long long consumer_scalar_wide(const ScalarRoot* value);
extern "C" void consumer_scalar_write(
    ScalarRoot& value, char letter, int word, long long wide);
extern "C" int consumer_scalar_first(const ScalarRoot* value);
extern "C" int consumer_scalar_second(const ScalarRoot& value);

extern "C" int consumer_fieldless_size();
extern "C" int consumer_fieldless_alignment();
extern "C" int consumer_fieldless_probe(const FieldlessRoot* value);

extern "C" int consumer_scalar_size() {
    return sizeof(ScalarRoot);
}

extern "C" int consumer_scalar_alignment() {
    return alignof(ScalarRoot);
}

extern "C" char consumer_scalar_letter(const ScalarRoot* value) {
    return value->letter;
}

extern "C" int consumer_scalar_word(const ScalarRoot& value) {
    return value.word;
}

extern "C" long long consumer_scalar_wide(const ScalarRoot* value) {
    return value->wide;
}

extern "C" void consumer_scalar_write(
    ScalarRoot& value, char letter, int word, long long wide) {
    value.letter = letter;
    value.word = word;
    value.wide = wide;
}

extern "C" int consumer_scalar_first(const ScalarRoot* value) {
    return value->first();
}

extern "C" int consumer_scalar_second(const ScalarRoot& value) {
    return value.second();
}

extern "C" int consumer_fieldless_size() {
    return sizeof(FieldlessRoot);
}

extern "C" int consumer_fieldless_alignment() {
    return alignof(FieldlessRoot);
}

extern "C" int consumer_fieldless_probe(const FieldlessRoot* value) {
    return value->probe();
}
