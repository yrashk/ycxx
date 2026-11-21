struct Box {
  int select() &;
};
int Box::select() && { return 1; } //! error
