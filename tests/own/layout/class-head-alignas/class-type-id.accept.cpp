// [dcl.align] Class-head alignment.
struct alignas(16) Basis { char value; };
struct alignas(Basis) Follower { char value; };

static_assert(alignof(Follower) == alignof(Basis));
static_assert(sizeof(Follower) == sizeof(Basis));
