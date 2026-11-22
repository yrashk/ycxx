// [expr.unary.op], [expr.mptr.oper], [class.access], [conv.mem]
struct Record { int field; static int shared; };
Record object{0};
int Record::* parenthesized = &(Record::field); //! error
int Record::* object_address = &object.field; //! error
int Record::* static_address = &Record::shared; //! error

class Hidden { int field; };
int Hidden::* private_address = &Hidden::field; //! error

struct Bits { int field : 3; };
int Bits::* bit_field_address = &Bits::field; //! error

struct Reference { int& field; };
int Reference::* reference_address = &Reference::field; //! error

struct Qualified { const int field; };
int Qualified::* unqualified_address = &Qualified::field; //! error

struct Other { int field; };
int read(Other other) {
  return other.*&Record::field; //! error
}

int Record::* computed_zero = 0 + 0; //! error
int Record::* bool_zero = false; //! error

struct Base { protected: int field; };
struct Derived : Base {
  int Base::* address() { return &Base::field; } //! error
};
