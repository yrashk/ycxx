struct Record {
    enum class Mode : unsigned char { idle, ready } active = Mode::ready, next{Mode::idle};
    char guard;
};
static_assert(sizeof(Record) == 3);
int verify() {
    Record value{};
    return value.active != Record::Mode::ready || value.next != Record::Mode::idle || value.guard != 0;
}
