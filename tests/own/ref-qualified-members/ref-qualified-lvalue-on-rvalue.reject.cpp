struct Box {
  int select() &;
};
int test(Box& box) { return static_cast<Box&&>(box).select(); } //! error
