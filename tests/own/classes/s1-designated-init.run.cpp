// δ s1 (#86): designated initializers, in declaration order, with omitted members zero or
// taking their default member initializer, nested designated lists ([dcl.init.aggr]).
// EXIT: 0
struct Config {
  int width;
  int height;
  int depth = 8;
  bool verbose;
};

struct Window { Config cfg; int id; };

int main() {
  Config a{.width = 640, .height = 480};
  if (a.width != 640 || a.height != 480 || a.depth != 8 || a.verbose) return 1;
  Config b = {.height = 2, .verbose = true};
  if (b.width != 0 || b.height != 2 || b.depth != 8 || !b.verbose) return 2;
  Config c{.width = 1, .depth = 24};
  if (c.depth != 24 || c.height != 0) return 3;
  Window w{.cfg = {.width = 3}, .id = 7};
  if (w.cfg.width != 3 || w.cfg.depth != 8 || w.id != 7) return 4;
  Window v{.id = 9};
  if (v.cfg.width != 0 || v.cfg.depth != 8 || v.id != 9) return 5;
  Config d{.depth{4}};
  if (d.depth != 4 || d.width != 0) return 6;
  return 0;
}
