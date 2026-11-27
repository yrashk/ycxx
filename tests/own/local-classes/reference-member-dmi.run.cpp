// Local classes (#117), spec-only: a local aggregate with a reference member bound to an
// enclosing automatic variable by aggregate initialization, and default member initializers used
// for the omitted members ([class.local], [dcl.init.aggr], [class.mem.general]).
int main() {
  int target = 1;
  struct R {
    int& ref;
    int scale = 3;
    int bias = 4;
  };
  R r{target};
  if (r.scale != 3 || r.bias != 4) return 1;
  r.ref = 10;
  if (target != 10) return 2;
  R s{target, 5};
  if (s.scale != 5 || s.bias != 4) return 3;
  s.ref = s.ref * s.scale + s.bias;
  if (target != 54) return 4;
  return 0;
}
