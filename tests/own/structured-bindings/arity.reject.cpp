// #285, spec-only. Each non-static data member takes one binding ([dcl.struct.bind]).
struct Position {
  int row;
  int column;
};

void bind(Position& position) {
  auto& [row] = position; //! error
}
