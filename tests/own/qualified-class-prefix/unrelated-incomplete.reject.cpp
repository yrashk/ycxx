// [basic.scope.class], [class.member.lookup]
struct Other;
struct Box {
  static constexpr int value = Other::member; //! error
};
