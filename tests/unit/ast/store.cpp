// The interners' storage (#107, ycxx.ast.store): block edges, ranges longer than a block,
// empty ranges, sparse columns, moves, and the fingerprint index.
import std;
import ycxx.ast;
import ycxx.unit;

using namespace ycxx::ast;
using ycxx::unit::check;

int main() {
  // Dense indices across the first block boundaries (blocks of 1024, 2048, 4096 units).
  Column<u32> dense;
  for (u32 i = 0; i < 8000; ++i) check(dense.push(i * 3) == i, "push gives dense indices");
  bool round_trip = true;
  for (u32 i : {0u, 1023u, 1024u, 3071u, 3072u, 7167u, 7168u, 7999u}) round_trip = round_trip && dense.get(i) == i * 3;
  check(round_trip, "values read back at the block edges");

  // Ranges: longer than the next block, after a partly used block, and empty.
  Column<u32> ranges;
  std::vector<u32> small(700), huge(5000), big(3000);
  std::iota(small.begin(), small.end(), 0u);
  std::iota(huge.begin(), huge.end(), 10000u);
  std::iota(big.begin(), big.end(), 20000u);
  const u32 a = ranges.append(std::span<const u32>(small));
  const u32 b = ranges.append(std::span<const u32>(huge));  // skips blocks 0 and 1's room
  const u32 e = ranges.append(std::span<const u32>{});
  const u32 c = ranges.append(std::span<const u32>(big));
  check(std::ranges::equal(ranges.span(a, 700), small) && std::ranges::equal(ranges.span(b, 5000), huge) &&
            std::ranges::equal(ranges.span(c, 3000), big),
        "every range is one contiguous span, whatever its length");
  check(ranges.span(e, 0).empty() && ranges.size() >= c + 3000, "an empty range; size is a high-water mark");

  // A sparse column: a block nobody pushed into.
  Column<std::uint64_t> sparse;
  sparse.put(100000, 42);
  sparse.put(5, 7);
  check(sparse.get(100000) == 42 && sparse.get(5) == 7, "put through ensure");

  // Moves (compaction, at quiescence).
  Chunked from(sizeof(u32));
  const u32 at = from.alloc(3000);
  for (u32 i = 0; i < 3000; ++i) std::memcpy(from.at(at + i), &i, sizeof i);
  Chunked to(std::move(from));
  u32 v = 0;
  std::memcpy(&v, to.at(at + 2999), sizeof v);
  check(v == 2999 && from.size() == 0, "move construction takes the blocks");
  Chunked again(sizeof(u32));
  (void)again.alloc(10);
  again = std::move(to);
  std::memcpy(&v, again.at(at + 1234), sizeof v);
  check(v == 1234 && to.size() == 0, "move assignment takes the blocks");

  // The index: one insertion per fingerprint.
  FpIndex<u32> index;
  int made = 0;
  const auto [x, fresh] = index.find_or_insert(seed("x"), [&] { return static_cast<u32>(++made); });
  const auto [y, again_fresh] = index.find_or_insert(seed("x"), [&] { return static_cast<u32>(++made); });
  check(fresh && !again_fresh && x == y && made == 1 && index.find(seed("x")) == x && !index.find(seed("y")),
        "find_or_insert makes a value once");
  return ycxx::unit::result("ast.store");
}
