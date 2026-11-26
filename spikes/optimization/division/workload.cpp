using Word = unsigned long long;

extern "C" Word shard_index(Word index, Word shard_size, Word salt) {
  return ((index / shard_size) ^ salt) + 3 * (index % shard_size);
}

extern "C" Word page_offset(Word index, Word salt) {
  return ((index / 4096ULL) ^ salt) + 3 * (index % 4096ULL);
}

extern "C" Word lane_index(unsigned index, unsigned width, unsigned salt) {
  return ((index / width) ^ salt) + 3 * (index % width);
}

extern "C" Word disjoint(Word index, Word other, Word width) {
  return (index / width) ^ (other % width);
}

extern "C" Word volatile_load(volatile Word* index, Word width) {
  const Word quotient = *index / width;
  return quotient ^ (*index % width);
}

extern "C" Word changed(Word* index, Word width) {
  const Word quotient = *index / width;
  *index += 4;
  return quotient ^ (*index % width);
}

extern "C" Word conditional(Word index, Word width, bool quotient) {
  if (quotient) return index / width;
  return index % width;
}
