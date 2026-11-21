// [conv.fctptr] does not allow dropping noexcept through multiple indirections.
using Safe = int() noexcept;
using Plain = int();
Plain** weaken(Safe** source) { return source; } //! error
