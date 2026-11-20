// [basic.lookup.argdep] (#165)
// EXIT: 0
namespace opaque {
struct Handle;

int probe(Handle* pointer) {
    return pointer == nullptr ? 81 : -1;
}
}

int main() {
    opaque::Handle* pointer = nullptr;
    return probe(pointer) == 81 ? 0 : 1;
}
