namespace shared { struct Tag { int key; }; }

int choose(const shared::Tag& tag);

int main() {
  shared::Tag tag{3};
  return ::choose(tag) == 43 ? 0 : 1;
}
