// [dcl.type.elab] performs type-only lookup for the elaborated name.
namespace language {
struct Token {
  int value;
};

namespace lexing {
int Token;

struct Scanner {
  struct Token* current;
  struct language::Token* saved;
};

int read(Scanner scanner) {
  language::Token* token = scanner.current;
  return token->value + scanner.saved->value;
}
}
}
