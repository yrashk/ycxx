// [basic.scope.class]: an enumerator initializer sees the class declarations in its prefix.
struct Later {
  enum class State { value = later }; //! error
  static constexpr int later = 7;
};

// [class.mfct.non.static]: no implicit object is available to the initializer.
struct Nonstatic {
  int member;
  enum State { value = member }; //! error
};
