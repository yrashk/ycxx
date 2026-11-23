struct Tag {
  explicit Tag(int& evaluated) noexcept;
};

struct Choice {
  int value;
  Choice(Tag tag, const Tag* original, int&& input,
         int a, int b, int c, int d, int e, int f) noexcept;
};
