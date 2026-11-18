// δ s4 (#86): a traffic-light state machine: enums as struct members, parameters and return
// values, an enum nested in a class named as `Class::enumerator`, enums with a fixed
// underlying type ([dcl.enum], [class.mem], [basic.lookup.qual]).
// EXIT: 0
enum Light : unsigned char { stop, ready, go, caution };

struct Signal {
  enum Mode { automatic, manual };
  Light light;
  Mode mode;
  int ticks;
};

Light next(Light l) {
  if (l == stop) return ready;
  if (l == ready) return go;
  if (l == go) return caution;
  return stop;
}

void step(Signal* s) {
  if (s->mode == Signal::manual) return;
  s->light = next(s->light);
  s->ticks++;
}

int main() {
  Signal s = {stop, Signal::automatic, 0};
  for (int i = 0; i < 6; i++) step(&s);
  if (s.light != go) return 1;
  if (s.ticks != 6) return 2;
  s.mode = Signal::manual;
  step(&s);
  if (s.light != go || s.ticks != 6) return 3;
  Signal::Mode m = s.mode;
  if (m != Signal::manual) return 4;
  Light l = caution;
  unsigned char raw = l;
  if (raw != 3) return 5;
  if (next(caution) != stop) return 6;
  Light lights[4] = {stop, ready, go, caution};
  int sum = 0;
  for (int i = 0; i < 4; i++) sum += lights[i];
  if (sum != 6) return 7;
  return 0;
}
