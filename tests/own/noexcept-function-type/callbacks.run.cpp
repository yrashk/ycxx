// [conv.fctptr], [expr.type], [over.ics.rank]
using Safe = int(int) noexcept;
using Plain = int(int);
int choose(Safe* callback) { return callback(4) + 11; }
int choose(Plain* callback) { return callback(4) + 21; }
int safe(int value) noexcept { return value + 1; }
int plain(int value) { return value + 2; }
static_assert(__is_same(decltype(true ? &safe : &plain), Plain*));
int main() {
    Plain* weakened = &safe;
    return choose(&safe) == 16 && choose(&plain) == 27 &&
           choose(weakened) == 26 && &safe == weakened ? 0 : 1;
}
