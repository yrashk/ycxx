// How deep this thread's recursion is (#187): analysis recurses as deeply as its input nests, and
// a pathological input (two million nested parentheses) would overflow any stack. A recursive
// entry point asks `deep()` and diagnoses instead of crashing; the threads that analyse have large
// stacks (engine::Pool::worker_stack), so a real program never gets near it.
export module ycxx.stack;

export import :bounds;
import std;

export namespace ycxx::stack {

// Whether more than half of this thread's stack is in use. The parser asks it; the passes after
// the parse recurse over the same nesting and are not guarded yet (a later pass may use more stack
// per level: #260).
bool deep() noexcept {
  static thread_local const std::pair<std::uintptr_t, std::uintptr_t> b = bounds();
  if (b.second <= b.first) return false;  // unknown
  char here = 0;
  const auto at = reinterpret_cast<std::uintptr_t>(&here);
  return at < b.first + (b.second - b.first) / 2;
}

}  // namespace ycxx::stack
