// #337 (ι): a goto out of blocks destroys the automatic objects of the blocks left, in reverse
// order of construction ([stmt.dcl]/2, [stmt.jump.general]/2); objects of blocks not left survive.
// EXIT: 0
int destroyed = 0;
int order = 0;

struct Tracker {
  int id;
  Tracker(int i) : id(i) {}
  ~Tracker() {
    ++destroyed;
    order = order * 10 + id;
  }
};

int out_of_nested() {
  destroyed = 0;
  order = 0;
  {
    Tracker outer(1);
    {
      Tracker mid(2);
      {
        Tracker inner(3);
        goto out;
      }
    }
  }
out:
  if (destroyed != 3) return 1;
  if (order != 321) return 2;
  return 0;
}

int partial() {
  destroyed = 0;
  order = 0;
  {
    Tracker keep(1);
    {
      Tracker gone(2);
      Tracker also(3);
      goto next;
    }
  next:
    if (destroyed != 2) return 3;
    if (order != 32) return 4;
  }
  if (destroyed != 3) return 5;
  if (order != 321) return 6;
  return 0;
}

int from_loop() {
  destroyed = 0;
  order = 0;
  for (int i = 0; i < 10; ++i) {
    Tracker t(i);
    if (i == 4) goto after;
  }
after:
  if (destroyed != 5) return 7;
  if (order != 1234) return 8;
  return 0;
}

int main() {
  int r = out_of_nested();
  if (r) return r;
  r = partial();
  if (r) return r;
  r = from_loop();
  if (r) return r;
  return 0;
}
