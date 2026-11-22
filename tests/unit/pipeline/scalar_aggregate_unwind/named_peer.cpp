int selected();
int overridden();

int calls;
bool throwing;

int foreign(int input) {
  ++calls;
  if (throwing) throw input;
  return input * 2;
}

int main(int argc, char** argv) {
  const char mode = argc > 1 ? argv[1][0] : 'o';
  throwing = mode != 'o';
  try {
    if (mode == 'e') return overridden() != 123 || calls != 0;
    if (mode == 'o') return selected() != 14 || calls != 1;
    if (mode == 'x') selected();
  } catch (int value) {
    return mode != 'x' || value != 7 || calls != 1;
  }
  return 2;
}
