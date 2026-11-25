// [class.nest], [temp.inst], [temp.type]
template<unsigned Tag>
struct Tagged {
  struct Record {
    unsigned char value;
  };
};

using First = Tagged<3>::Record;
using Same = Tagged<1u + 2u>::Record;
using Other = Tagged<4>::Record;

static_assert(sizeof(First) == sizeof(Other));
static_assert(alignof(First) == alignof(Other));

// The declarations conflict if the two parameter types are the same.
int choose(First*);
long choose(Other*);

// Returning this pointer requires First and Same to name the same class.
Same* convert(First* value) { return value; }
