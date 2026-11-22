// [basic.scope.class], [class.member.lookup]
struct Box {
  static constexpr int value = Box::later; //! error
  static constexpr int later = 4;
};
