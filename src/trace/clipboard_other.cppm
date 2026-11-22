// The system clipboard without a program in between (#636): none here yet. On Linux, X11 and
// Wayland keep a clipboard only while its owner runs and serves it, which needs a decision on
// libraries and an owner process (#730); until then the browser runs a clipboard command
// (`:browse`). macOS's is clipboard_darwin.cppm.
export module ycxx.trace:clipboard;

import std;

export namespace ycxx::trace {

inline constexpr std::string_view native_clipboard = {};

bool pasteboard_write(std::string_view, std::string_view = {}) { return false; }
std::optional<std::string> pasteboard_read(std::string_view = {}) { return std::nullopt; }
void pasteboard_drop(std::string_view) {}

}  // namespace ycxx::trace
