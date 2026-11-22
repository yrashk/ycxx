struct Guard {
  int value;
  Guard(int n) noexcept : value(n) {}
  Guard(const Guard&) = delete;
  Guard(Guard&& other) noexcept : value(other.value) { other.value = 0; }
};

struct Owner { Guard guard; };

int main() {
  Owner origin{Guard(17)};
  Owner moved(static_cast<Owner&&>(origin));
  return moved.guard.value != 17 || origin.guard.value != 0;
}
