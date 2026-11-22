// [basic.scope.enum], [class.member.lookup]
namespace Prefix {
struct Box {
  enum State { first = 3, second = Box::first + 1 };
  static constexpr int after_enum = Box::second + 1;
};
static_assert(Box::first == 3);
static_assert(Box::second == 4);
static_assert(Box::after_enum == 5);
}
