int foreign(int);
void note(int) noexcept;

struct Slot { int input; int value = foreign(input); };
struct Guard {
  int stamp;
  ~Guard() noexcept { note(stamp); }
};

int guarded_default() {
  Guard guard{41};
  Slot slot{7};
  return slot.value;
}
