// [dcl.init.aggr], [dcl.init.list], [dcl.enum]
enum class Entry : unsigned char {};
struct Record { Entry member; };

constexpr Record direct{.member{37}};
constexpr Record typed{.member = Entry{41}};
static_assert(static_cast<unsigned>(direct.member) == 37);
static_assert(static_cast<unsigned>(typed.member) == 41);

int main() {
  Record value{.member{59}};
  return static_cast<unsigned>(value.member) != 59;
}
