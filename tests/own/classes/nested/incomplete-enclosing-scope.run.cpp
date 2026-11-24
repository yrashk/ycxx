// #1216 [class.nest]/1, [basic.scope.class]/1, [basic.lookup.unqual]/2–4,
// [class.member.lookup]/3–6, [class.mem.general]/10–11, [expr.prim.id.general]/2,4.
namespace model {
using Record = unsigned char[1];
struct Base { using Record = unsigned char[3]; };
int tag() { return 5; }

struct Outer : Base {
    static int tag();
    struct Inner {
        Record record;
        static constexpr auto qualified = &Outer::tag;
        static constexpr auto unqualified = &tag;
    };
};

int Outer::tag() { return 17; }
}

static_assert(sizeof(model::Outer::Inner::record) == 3);

int main() {
    const model::Outer::Inner item{{1, 2, 3}};
    return item.record[2] == 3 && item.qualified() == 17 && item.unqualified() == 17 ? 0 : 1;
}
