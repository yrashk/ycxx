// [dcl.align] Class-head alignment.
struct alignas(16) Cell { char tag; };

struct Envelope {
  char lead;
  Cell cells[2];
  char tail;
};

static_assert(alignof(Cell) == 16);
static_assert(sizeof(Cell) == 16);
static_assert(alignof(Cell[3]) == 16);
static_assert(sizeof(Cell[3]) == 48);
static_assert(alignof(Envelope) == 16);
static_assert(sizeof(Envelope) == 64);
