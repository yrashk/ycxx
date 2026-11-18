// δ s1 (#86): designators out of declaration order, naming no member, or repeated
// ([dcl.init.aggr]).
struct Config { int width; int height; int depth; };

int main() {
  Config a{.height = 1, .width = 2}; //! error
  Config b{.colour = 3}; //! error
  Config c{.width = 1, .width = 2}; //! error
  return a.width + b.width + c.width;
}
