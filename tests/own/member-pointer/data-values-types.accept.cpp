// [expr.unary.op], [expr.mptr.oper]
namespace N {
struct Record {
  int first;
  const int fixed;
  volatile int changing;
  static int shared;

  void address_forms() {
    static_assert(__is_same(decltype(&Record::first), int Record::*));
    static_assert(__is_same(decltype(&(Record::first)), int*));
    static_assert(__is_same(decltype(&first), int*));
  }
};
}

using Record = N::Record;
using Member = int Record::*;
using ConstRecord = const Record;
using ConstMember = const Member;
extern Record object;
extern ConstRecord const_object;
extern const Record* const_pointer;
extern Member first;

static_assert(__is_same(decltype(&::N::Record::first), Member));
static_assert(__is_same(decltype(&decltype(object)::first), Member));
static_assert(__is_same(decltype(&(Record::first)), int*));
static_assert(__is_same(decltype(&object.first), int*));
static_assert(__is_same(decltype(&Record::shared), int*));
static_assert(__is_same(decltype(&(Record::shared)), int*));
static_assert(__is_same(decltype(object.*first), int&));
static_assert(__is_same(decltype(object.*first + object.*first), int));
static_assert(__is_same(decltype(Record{3, 5, 7}.*first), int&&));
static_assert(__is_same(decltype(static_cast<Record&&>(object).*first), int&&));
static_assert(__is_same(decltype((&object)->*first), int&));
static_assert(__is_same(decltype(const_object.*first), const int&));
static_assert(__is_same(decltype(static_cast<ConstMember>(nullptr)), Member));
static_assert(__is_same(decltype(object.*&Record::fixed), const int&));
static_assert(__is_same(decltype(const_pointer->*&Record::changing), const volatile int&));

class Hidden {
  int field;

public:
  static int Hidden::* grant() { return &Hidden::field; }
};

int read(Hidden& object) { return object.*Hidden::grant(); }
void take(Member);
void initialize() { take(nullptr); }
void bind() { const int& field = Record{3, 5, 7}.*first; (void)field; }

struct Incomplete;
extern Incomplete* incomplete;
extern int Incomplete::* later;
static_assert(__is_same(decltype(incomplete->*later), int&));
