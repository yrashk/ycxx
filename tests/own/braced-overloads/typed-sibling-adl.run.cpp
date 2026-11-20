// [over.ics.list], [over.ics.rank], [dcl.init.list] (#231)
// EXIT: 0
namespace keys {
struct Key;

int choose(int value, const Key*) { return 10 + value; }
}

int choose(long value, const keys::Key*) { return 20 + static_cast<int>(value); }

int main() {
    const keys::Key* key = nullptr;
    if (choose({1}, key) != 11) return 1;
    if (choose({1L}, key) != 21) return 2;
    return 0;
}
