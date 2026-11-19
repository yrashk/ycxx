// #337 (ι): since C++23 an alias-declaration may be the init-statement of an if, switch or for
// statement ([stmt.pre]/1, [stmt.if], [stmt.switch], [stmt.for], P2360); the alias is in scope
// in the rest of the statement.
// EXIT: 0
int main() {
  int sum = 0;
  for (using T = int; T i = 0;) {
    sum = 100 + i;
    break;
  }
  if (sum != 0) return 1;

  for (using T = int; sum < 5;) {
    T step = 1;
    sum += step;
  }
  if (sum != 5) return 2;

  if (using T = int; T(1)) {
    T x = 3;
    sum += x;
  } else {
    return 3;
  }
  if (sum != 8) return 4;

  if (using T = long; sizeof(T) == sizeof(long)) {
  } else {
    return 5;
  }

  switch (using E = char; E('b')) {
    case 'a': return 6;
    case 'b': {
      E c = 'z';
      if (c != 'z') return 7;
      break;
    }
    default: return 8;
  }
  return 0;
}
