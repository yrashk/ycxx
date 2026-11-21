struct Box {
  int select() &&;
};
int test(Box& box) { return box.select(); } //! error
