struct Position {
  long row;
  long column;
  ~Position() = default;
};

Position advance(Position position, long offset);
int check_explicit_destruction();
extern int receiver_calls;
void destroy_position(Position* position);
