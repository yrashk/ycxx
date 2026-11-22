// [basic.scope.class], [class.member.lookup]
namespace Prefix {
constexpr int value = 17;
struct Box {
  static constexpr int value = 4;
  static constexpr int class_value = Prefix::Box::value;
  static constexpr int namespace_value = Prefix::value;
  static constexpr int first = 6, second = Box::first;
};
static_assert(Box::class_value == 4);
static_assert(Box::namespace_value == 17);
static_assert(Box::second == 6);
}
