// EXIT: 0
// [basic.scope.pdecl], [dcl.init.list], [dcl.constexpr], [expr.sizeof]

struct Width {
  decltype(sizeof(char)) bytes;
  char data;
};

char object;

class Holder {
public:
  static constexpr Width object{sizeof(object), 2};
};

const Width* volatile escaped_object;

int main() {
  escaped_object = &Holder::object;
  const Width* address = escaped_object;
  return address->bytes == sizeof(Width) ? 0 : 1;
}
