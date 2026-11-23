// #1030 [class.prop], [meta.unary.prop], [term.standard.layout.type].
struct Packet { int sequence; unsigned size; };
class PrivatePacket { int sequence; unsigned size; };
union ScalarChoice { int first; long second; };
struct VirtualMember { virtual int value(); };
struct ReferenceMember { int& value; };
struct MixedAccess {
  int first;
private:
  int second;
};
struct ContainsMixed { MixedAccess value; };
struct InheritsMixed : MixedAccess {};

static_assert(__is_standard_layout(Packet));
static_assert(__is_standard_layout(const volatile PrivatePacket));
static_assert(__is_standard_layout(ScalarChoice));
static_assert(!__is_standard_layout(VirtualMember));
static_assert(!__is_standard_layout(ReferenceMember));
static_assert(!__is_standard_layout(MixedAccess));
static_assert(!__is_standard_layout(ContainsMixed));
static_assert(!__is_standard_layout(InheritsMixed));

struct DataRoot { int value; };
struct OtherData { int other; };
struct InheritedData : DataRoot {};
struct AddsData : DataRoot { int own; };
struct TwoOrigins : DataRoot, OtherData {};

static_assert(__is_standard_layout(InheritedData));
static_assert(!__is_standard_layout(AddsData));
static_assert(!__is_standard_layout(TwoOrigins));

struct Empty {};
struct Left : Empty {};
struct Right : Empty {};
struct RepeatedBase : Left, Right {};
struct FirstMemberCollision : Empty { Empty first; };
struct LaterMember : Empty { int first; Empty second; };

static_assert(__is_standard_layout(Left));
static_assert(!__is_standard_layout(RepeatedBase));
static_assert(!__is_standard_layout(FirstMemberCollision));
static_assert(__is_standard_layout(LaterMember));

using OpenRows = const volatile int[][3];
using Function = int(int);

static_assert(__is_standard_layout(const volatile int));
static_assert(__is_standard_layout(OpenRows));
static_assert(__is_standard_layout(const Packet[2]));
static_assert(!__is_standard_layout(MixedAccess[2]));
static_assert(!__is_standard_layout(Packet&));
static_assert(!__is_standard_layout(Function));
static_assert(!__is_standard_layout(const volatile void));
