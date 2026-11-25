extern "C" int foreign_score_bonus();

int score() {
  return 35 + foreign_score_bonus();
}
