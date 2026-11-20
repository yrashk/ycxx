static int observed = 1;
auto by_reference() -> int& { return observed; }
short narrow() { return -7; }
int main() { by_reference() = 42; return observed == 42 && narrow() == -7 ? 0 : 1; }
