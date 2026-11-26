// Classes with const class members. Their own tail padding and that of the classes that contain them
// follow different rules on arm64 macOS.
struct Aggregate { int x; char y; };
template <class T> struct After : T { char last; };

// A class that contains one, or derives from one: its tail padding is not reused there.
struct Implicit { const Aggregate p; char c; };
struct Contains { Implicit i; char c; };
struct Derives : Implicit {};
template <class T> struct Wraps { T t; char w; };
struct ContainsTemplate { Wraps<const Aggregate> w; char c; };
union Overlaps { const Aggregate p; int z; };
struct ContainsUnion { Overlaps u; char c; };
struct ConstCallable { ConstCallable& operator=(const ConstCallable&) const = delete; int x; char y; };
struct HasConstCallable { const ConstCallable p; char c; };
constexpr int containing = 5;
static int containing_size(int i) {
  const int sizes[containing] = {sizeof(After<Contains>), sizeof(After<Derives>), sizeof(After<ContainsTemplate>),
                                 sizeof(After<ContainsUnion>), sizeof(After<HasConstCallable>)};
  return sizes[i];
}

// A class with a const class member of its own.
struct Constant { Constant() = default; const Aggregate p; char c; };
struct ConstantMove { ConstantMove() = default; ConstantMove& operator=(ConstantMove&&) = default; const Aggregate p; char c; };
struct ConstantArray { const Aggregate p[2]; char c; };
constexpr int direct = 4;
static int direct_size(int i) {
  const int sizes[direct] = {sizeof(After<Implicit>), sizeof(After<Constant>), sizeof(After<ConstantMove>),
                             sizeof(After<ConstantArray>)};
  return sizes[i];
}

int peer_containing_size(int i);
int peer_direct_size(int i);
int peer_last(const After<Contains>* a);
