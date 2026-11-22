void first() noexcept;
void first() noexcept(3 == 3) {}
int second(char);
int second(char) noexcept(false) { return 0; }
