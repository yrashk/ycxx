// δ s4 (#86): unscoped enumerators share the enclosing scope, so a repeated enumerator name
// clashes; an enumerator value that does not fit the fixed underlying type is narrowing
// ([dcl.enum], [basic.scope.scope], [dcl.init.list] narrowing).
enum Fruit { apple, pear };
enum Tree { oak, apple }; //! error

enum Small : unsigned char { tiny = 1, huge = 300 }; //! error

enum class Ok { apple, pear };  // scoped: no clash

int main() { return 0; }
