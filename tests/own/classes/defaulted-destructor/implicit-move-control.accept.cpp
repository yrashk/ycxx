// #157 [class.copy.ctor]: an implicit move can initialize an rvalue reference member.
struct MoveControl {
  int&& reference;
};

void move_control(MoveControl&& source) {
  MoveControl result(static_cast<MoveControl&&>(source));
}
