// Namespace-scope opaque enumerations ([dcl.enum], #157).
enum class Code;
enum Flag : unsigned char;
static_assert(sizeof(Code) == sizeof(int));
static_assert(alignof(Flag) == alignof(unsigned char));
Code echo(Code code) { return code; }
enum struct Code : int { answer = 42 };
enum Flag : unsigned char { flag = 7 };
enum class Code;
enum Flag : unsigned char;
int main() {
  return echo(Code::answer) == Code::answer && flag == 7 ? 0 : 1;
}
