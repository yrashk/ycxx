int via_direct();
int via_explicit_temporary();
int via_literal_temporary();

int calls;
bool throwing;

int exception_source(int input) {
  ++calls;
  if (throwing) throw input;
  return input * 2;
}

int main(int argc, char** argv) {
  const char mode = argc > 1 ? argv[1][0] : 'n';
  throwing = mode != 'n';
  try {
    if (mode == 'n') return via_explicit_temporary() != 14 || calls != 1;
    if (mode == 'l') return via_literal_temporary() != 19 || calls != 0;
    if (mode == 'd') via_direct();
    if (mode == 't') via_explicit_temporary();
  } catch (int value) {
    return (mode != 'd' && mode != 't') || value != 7 || calls != 1;
  }
  return 2;
}
