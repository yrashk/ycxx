int foreign(int);

struct Slot { int input; int value = foreign(input); };

int selected() {
  Slot slot{7};
  return slot.value;
}

int overridden() {
  Slot slot{7, 123};
  return slot.value;
}
