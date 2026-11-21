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

extern "C" int printf(const char* format, ...);

static_assert(sizeof(void*) == 8 && alignof(void*) == 8);
static_assert(sizeof(int) == 4 && alignof(int) == 4);
static_assert(sizeof(long long) == 8 && alignof(long long) == 8);

int ScalarRoot::first() const {
    return 107;
}

int ScalarRoot::second() const {
    return 313;
}

int FieldlessRoot::probe() const {
    return 619;
}

int check(const char* name, long long actual, long long expected) {
    printf("%s actual=%lld expected=%lld\n", name, actual, expected);
    return actual != expected;
}

int main() {
    int failures = 0;
    failures += check("peer ScalarRoot sizeof", sizeof(ScalarRoot), 24);
    failures += check("peer ScalarRoot alignof", alignof(ScalarRoot), 8);
    failures += check("consumer ScalarRoot sizeof", consumer_scalar_size(), 24);
    failures += check("consumer ScalarRoot alignof", consumer_scalar_alignment(), 8);
    failures += check("peer FieldlessRoot sizeof", sizeof(FieldlessRoot), 8);
    failures += check("peer FieldlessRoot alignof", alignof(FieldlessRoot), 8);
    failures += check("consumer FieldlessRoot sizeof", consumer_fieldless_size(), 8);
    failures += check("consumer FieldlessRoot alignof", consumer_fieldless_alignment(), 8);
    if (failures != 0) {
        return 1;
    }

    const char initial_letter = 0x51;
    const int initial_word = 0x13579bdf;
    const long long initial_wide = 0x0123456789abcdefLL;
    ScalarRoot scalar;
    scalar.letter = initial_letter;
    scalar.word = initial_word;
    scalar.wide = initial_wide;
    FieldlessRoot fieldless;

    failures += check("consumer letter from peer", consumer_scalar_letter(&scalar), initial_letter);
    failures += check("consumer word from peer", consumer_scalar_word(scalar), initial_word);
    failures += check("consumer wide from peer", consumer_scalar_wide(&scalar), initial_wide);
    failures += check("consumer ScalarRoot first", consumer_scalar_first(&scalar), 107);
    failures += check("consumer ScalarRoot second", consumer_scalar_second(scalar), 313);
    failures += check("consumer FieldlessRoot probe", consumer_fieldless_probe(&fieldless), 619);
    if (failures != 0) {
        return 1;
    }

    const char updated_letter = 0x78;
    const int updated_word = 0x2468ace0;
    const long long updated_wide = 0x07654321fedcba98LL;
    consumer_scalar_write(scalar, updated_letter, updated_word, updated_wide);
    failures += check("peer letter from consumer", scalar.letter, updated_letter);
    failures += check("peer word from consumer", scalar.word, updated_word);
    failures += check("peer wide from consumer", scalar.wide, updated_wide);
    if (failures != 0) {
        return 1;
    }

    failures += check("consumer ScalarRoot first after writes", consumer_scalar_first(&scalar), 107);
    failures += check("consumer ScalarRoot second after writes", consumer_scalar_second(scalar), 313);
    return failures == 0 ? 0 : 1;
}
