namespace protocol {
enum { before_linkage = 7 };

extern "C" {
enum { inside_linkage = before_linkage + 1, inside_next };
static_assert(inside_linkage == 8);
static_assert(inside_next == 9);
}

enum { after_linkage = inside_next + 1 };
static_assert(after_linkage == 10);
}

static_assert(protocol::inside_linkage == 8);
static_assert(protocol::inside_next == 9);
