// Namespace-scope opaque enumerations ([dcl.enum], #157).
enum class Color : unsigned char { red = 1 };
enum struct Color : unsigned char;
enum Bit : unsigned { one = 1 };
enum Bit : unsigned;
static_assert(sizeof(Color) == sizeof(unsigned char));
int main() { return static_cast<unsigned char>(Color::red) == 1 && one == 1 ? 0 : 1; }
