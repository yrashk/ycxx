// REQUIRES: linux
// PARITY: #294

struct B { virtual int identify(); };

using QualifiedB = const volatile B;
using QualifiedVoid = const volatile void;

static_assert(dynamic_cast<void*>(static_cast<B*>(nullptr)) == nullptr);
static_assert(dynamic_cast<QualifiedVoid*>(static_cast<QualifiedB*>(nullptr)) == nullptr);

struct Mutable { char bytes[1]; };
struct Const { char bytes[2]; };
struct Volatile { char bytes[3]; };
struct ConstVolatile { char bytes[4]; };

Mutable qualifier(void*);
Const qualifier(const void*);
Volatile qualifier(volatile void*);
ConstVolatile qualifier(const volatile void*);

static_assert(sizeof(qualifier(dynamic_cast<void*>(static_cast<B*>(nullptr)))) == 1);
static_assert(sizeof(qualifier(dynamic_cast<const void*>(static_cast<B*>(nullptr)))) == 2);
static_assert(sizeof(qualifier(dynamic_cast<volatile void*>(static_cast<volatile B*>(nullptr)))) == 3);
static_assert(sizeof(qualifier(dynamic_cast<QualifiedVoid*>(static_cast<QualifiedB*>(nullptr)))) == 4);
