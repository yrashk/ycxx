// Frames larger than a page, compiled by ycxx (#644).
// Every L2 value has its own 8-byte slot at level 0, so element-wise initialization makes
// these frames several times larger than their arrays.
extern "C" int large_frame(int index, int value) {
  int words[16384];  // 64 KiB of array; the frame is over 400 KiB
  words[index] = value;
  words[16383 - index] = value + 1;
  return words[index] + words[16383 - index];
}

extern "C" int deep(int depth) {
  char bytes[700];  // a frame of several pages with a partial last page
  bytes[depth % 700] = static_cast<char>(depth);
  if (depth == 0) return bytes[0];
  return deep(depth - 1) + (bytes[depth % 700] == static_cast<char>(depth) ? 1 : 1000);
}
