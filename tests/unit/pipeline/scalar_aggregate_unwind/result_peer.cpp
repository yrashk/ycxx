#include "result.hpp"

int calls;
bool throwing;

int result_field(int input) {
  ++calls;
  if (throwing) throw input;
  return 2 * input;
}

int main(int argc, char** argv) {
  if (argc != 2) return 3;
  const char mode = argv[1][0];
  throwing = mode == 'S' || mode == 'P' || mode == 'W';
  try {
    if (mode == 's' || mode == 'S') {
      const Small result = small_result(13);
      return throwing || result.value != 26 || calls != 1;
    }
    if (mode == 'p' || mode == 'P') {
      const Pair result = named_result(17);
      return throwing || result.first != 34 || result.second != 19 || calls != 1;
    }
    if (mode == 'w' || mode == 'W') {
      const Wide result = wide_result(23);
      return throwing || result.first != 11 || result.second != 46 || result.third != 37 || calls != 1;
    }
  } catch (int value) {
    const int expected = mode == 'S' ? 13 : mode == 'P' ? 17 : 23;
    return !throwing || value != expected || calls != 1;
  }
  return 2;
}
