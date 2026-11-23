// #975 [stmt.return], [class.copy.elision]
struct Text {
  char bytes[4];

  constexpr char first() const { return bytes[0]; }
  constexpr const char* middle() const { return bytes + 1; }
};

constexpr Text make_text() {
  Text value{{'a', 'b', 'c', '\0'}};
  return value;
}
