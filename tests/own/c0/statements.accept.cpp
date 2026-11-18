// C0 statements (#39, #41): compound, expression, selection, iteration and jump statements
// ([stmt]), with the block scopes they introduce ([basic.scope.block]).
int count(int n) {
  int total = 0;
  for (int i = 0; i < n; ++i) {
    if (i % 2 == 0) continue;
    total += i;
  }
  for (;;) break;
  int j = 0;
  for (j = 0; j < 3; j++) total += j;
  for (; j > 0;) --j;
  while (total > 100) total -= 7;
  while (true) {
    if (total < 0) break;
    total = total - 1;
    if (total < 50) break;
  }
  do total++;
  while (total < 10);
  do {
    total += 2;
  } while (false);
  if (total) {
  } else if (n) {
    ;
  } else {
    total = 0;
  }
  {
    int total = 3;  // hides the outer one
    total = total + 1;
  }
  ;
  {}
  return total;
}
int main() {
  int i = 5;
  if (i > 2)
    i = 2;
  else
    i = 3;
  while (i)
    --i;
  return count(10) + i;
}
