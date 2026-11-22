struct Guard { int value; ~Guard() noexcept {} };
struct Owner { Guard guard; };
Owner acquire() noexcept { return Owner{{42}}; }
int main() { auto owner = acquire(); return owner.guard.value - 42; }
