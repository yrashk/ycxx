constexpr int ordinary_constant = 7;
extern constexpr int external_constant = 9;
static inline int local_inline = 11;
struct Ordinary { static int value; };
int Ordinary::value = 13;
int backing = 13;
inline int* relocated = &backing;
template<class T> inline int specialized = 17;
template<> int specialized<int> = 19;
struct NamedType {};
template<> inline int specialized<NamedType> = 23;

namespace {
struct Payload { int n; };
enum Kind { one = 1 };
}

struct Public {
    static constexpr Payload p = Payload{42};
    static constexpr Kind k = one;
    static constexpr int scalar = 42;
};

const void* payload_address() { return &Public::p; }
const void* kind_address() { return &Public::k; }
const void* scalar_address() { return &Public::scalar; }
