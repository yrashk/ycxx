// [dcl.type.cv]: const objects can have writable mutable subobjects.
struct Counter {
    mutable int count;
    int tick() const { return ++count; }
};
struct Nest { Counter counters[2]; };

const Nest global = {{{0}, {1}}};
const Nest zero = {};
struct Frozen { int value; };
extern const Frozen ordinary = {3};

int main() {
    global.counters[1].count += 1;
    int changed = global.counters[1].tick();
    int changed_zero = zero.counters[0].tick();
    return changed == ordinary.value && changed_zero == 1 ? 0 : 1;
}
