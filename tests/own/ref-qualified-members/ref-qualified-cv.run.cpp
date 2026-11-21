// EXIT: 0
namespace fixture {
struct Box {
  int select() & { return 1; }
  int select() const & { return 2; }
  int select() volatile & { return 3; }
  int select() const volatile & { return 4; }
  int select() && { return 5; }
  int select() const && { return 6; }
  int select() volatile && { return 7; }
  int select() const volatile && { return 8; }

  int fallback() const & { return 9; }
  int mixed() { return 10; }
  int mixed() const & { return 11; }
};
}

int main() {
  fixture::Box plain;
  const fixture::Box constant{};
  volatile fixture::Box changing;
  const volatile fixture::Box both{};
  if (plain.select() != 1) return 1;
  if (constant.select() != 2) return 2;
  if (changing.select() != 3) return 3;
  if (both.select() != 4) return 4;
  if (static_cast<fixture::Box&&>(plain).select() != 5) return 5;
  if (static_cast<const fixture::Box&&>(constant).select() != 6) return 6;
  if (static_cast<volatile fixture::Box&&>(changing).select() != 7) return 7;
  if (static_cast<const volatile fixture::Box&&>(both).select() != 8) return 8;
  if (fixture::Box{}.fallback() != 9) return 9;
  if (fixture::Box{}.mixed() != 10) return 10;
  if (constant.mixed() != 11) return 11;
  return 0;
}
