struct Record {
    int first;
    int second;
};

constexpr int Record::* first = &Record::first;
constexpr int Record::* second = &Record::second;
constexpr int Record::* value_initialized{};
constexpr int Record::* explicit_null = nullptr;
constexpr int Record::* literal_zero = (0);
constexpr int Record::* copy = (second);
