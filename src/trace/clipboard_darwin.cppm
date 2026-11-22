// The system clipboard without a program in between (#636), macOS: NSPasteboard, reached through
// the Objective-C runtime's C entry points. libobjc and AppKit are loaded at the first copy, not at
// startup, so a compile that never copies pays nothing; no Objective-C header is parsed. The other
// platforms' file is clipboard_other.cppm (Linux: #730).
module;
#include <dlfcn.h>
export module ycxx.trace:clipboard;

import std;

export namespace ycxx::trace {

// Whether this host has a clipboard of its own the browser writes directly, and its name for the
// status line.
inline constexpr std::string_view native_clipboard = "pasteboard";

// Puts `text` on the pasteboard `name` (empty: the general one, the user's clipboard); false when
// the system has none for this process or refused it.
bool pasteboard_write(std::string_view text, std::string_view name = {});
// The text a pasteboard holds, if any (for tests).
std::optional<std::string> pasteboard_read(std::string_view name = {});
// Forgets a named pasteboard (for tests).
void pasteboard_drop(std::string_view name);

}  // namespace ycxx::trace

namespace ycxx::trace {
namespace {

using id = void*;
using sel_t = void*;

struct Runtime {
  id (*get_class)(const char*) = nullptr;
  sel_t (*sel)(const char*) = nullptr;
  void* send = nullptr;  // objc_msgSend, cast to each message's own signature
  bool ok = false;
};
const Runtime& runtime() {
  static const Runtime r = [] {
    Runtime out;
    void* objc = ::dlopen("/usr/lib/libobjc.A.dylib", RTLD_LAZY | RTLD_GLOBAL);
    void* appkit = ::dlopen("/System/Library/Frameworks/AppKit.framework/AppKit", RTLD_LAZY | RTLD_GLOBAL);
    if (!objc || !appkit) return out;
    out.get_class = reinterpret_cast<id (*)(const char*)>(::dlsym(objc, "objc_getClass"));
    out.sel = reinterpret_cast<sel_t (*)(const char*)>(::dlsym(objc, "sel_registerName"));
    out.send = ::dlsym(objc, "objc_msgSend");
    out.ok = out.get_class && out.sel && out.send;
    return out;
  }();
  return r;
}
// `[self selector args…]`, through objc_msgSend called with the message's exact prototype (on
// arm64 it must not be called as a variadic function). A message to nil answers 0.
template <class R, class... A>
R msg(id self, const char* selector, A... args) {
  const Runtime& r = runtime();
  return reinterpret_cast<R (*)(id, sel_t, A...)>(r.send)(self, r.sel(selector), args...);
}
id cls(const char* name) { return runtime().get_class(name); }
// A new NSString of UTF-8 bytes (nil for bytes that are not UTF-8); the caller releases it.
id string(std::string_view s) {
  constexpr unsigned long utf8 = 4;  // NSUTF8StringEncoding
  return msg<id>(msg<id>(cls("NSString"), "alloc"), "initWithBytes:length:encoding:", static_cast<const void*>(s.data()),
                 static_cast<unsigned long>(s.size()), utf8);
}
// What the messages autorelease (the pasteboard objects) is released when this goes.
class Pool {
 public:
  Pool() : pool_(msg<id>(msg<id>(cls("NSAutoreleasePool"), "alloc"), "init")) {}
  ~Pool() { msg<void>(pool_, "drain"); }
  Pool(const Pool&) = delete;

 private:
  id pool_;
};
id board(std::string_view name) {
  if (name.empty()) return msg<id>(cls("NSPasteboard"), "generalPasteboard");
  const id n = string(name);
  const id b = msg<id>(cls("NSPasteboard"), "pasteboardWithName:", n);
  msg<void>(n, "release");
  return b;
}
constexpr std::string_view text_type = "public.utf8-plain-text";  // NSPasteboardTypeString

}  // namespace

bool pasteboard_write(std::string_view text, std::string_view name) {
  if (!runtime().ok) return false;
  Pool pool;
  const id b = board(name);
  if (!b) return false;
  (void)msg<long>(b, "clearContents");
  const id s = string(text), type = string(text_type);
  const bool ok = s && type && msg<bool>(b, "setString:forType:", s, type);
  msg<void>(s, "release");
  msg<void>(type, "release");
  return ok;
}

std::optional<std::string> pasteboard_read(std::string_view name) {
  if (!runtime().ok) return std::nullopt;
  Pool pool;
  const id b = board(name);
  const id type = string(text_type);
  const id s = msg<id>(b, "stringForType:", type);
  msg<void>(type, "release");
  const char* utf8 = s ? msg<const char*>(s, "UTF8String") : nullptr;
  return utf8 ? std::optional<std::string>(utf8) : std::nullopt;
}

void pasteboard_drop(std::string_view name) {
  if (!runtime().ok || name.empty()) return;
  Pool pool;
  msg<void>(board(name), "releaseGlobally");
}

}  // namespace ycxx::trace
