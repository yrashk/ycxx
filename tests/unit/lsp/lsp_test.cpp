// The LSP front's session (#99, daemon design §6.4): framing, JSON and its reflection-derived
// codec, URIs, and a session driven by an editor's messages, with a fake `Diagnose`.
import std;
import ycxx.lsp;
import ycxx.unit;

using namespace ycxx::lsp;
using ycxx::unit::check;

namespace {

// An editor's messages in, the server's out. `pause` ends a batch: the editor waits there, so
// `pending` says nothing more is ready.
class Script final : public Channel {
public:
  void send(const Json& msg) {
    const std::string body = dump(msg);
    in_ += std::format("Content-Length: {}\r\n\r\n{}", body.size(), body);
  }
  void pause() { pauses_.push_back(in_.size()); }
  std::size_t read(std::span<char> buf) override {
    const std::size_t n = std::min(buf.size(), in_.size() - at_);
    std::copy_n(in_.data() + at_, n, buf.data());
    at_ += n;
    return n;
  }
  bool pending() override { return at_ < in_.size() && !std::ranges::contains(pauses_, at_); }
  bool write(std::string_view bytes) override {
    out_ += bytes;
    return true;
  }
  // The server's messages, parsed.
  std::vector<Json> replies() {
    Script back;
    back.in_ = out_;
    std::vector<Json> r;
    while (auto m = read_message(back)) r.push_back(*parse(*m));
    return r;
  }

private:
  std::string in_, out_;
  std::size_t at_ = 0;
  std::vector<std::size_t> pauses_;
};

Json request(int id, std::string method, Json params = Json(Object{})) {
  return Json(Object{{"jsonrpc", "2.0"}, {"id", id}, {"method", std::move(method)}, {"params", std::move(params)}});
}
Json notification(std::string method, Json params) {
  return Json(Object{{"jsonrpc", "2.0"}, {"method", std::move(method)}, {"params", std::move(params)}});
}

void json() {
  auto j = parse(R"({"a": [1, -2.5, true, null, "x\"\u00e9\ud83d\ude00"], "b": {}})");
  check(j.has_value() && (*j)["a"]->array()->size() == 5, "parse");
  check(*(*(*j)["a"]->array())[4].string() == "x\"\u00e9\U0001F600", "escapes and surrogate pairs");
  check(dump(*parse(dump(*j))) == dump(*j), "dump round-trips");
  for (const char* bad : {"{", "[1,]", "\"\\q\"", "01x", "{\"a\" 1}", "\"\\ud800\"", "[1] 2"}) {
    check(!parse(bad).has_value(), std::format("rejected: {}", bad));
  }
  struct Inner {
    int n = 0;
  };
  struct Outer {
    std::string s;
    std::optional<int> maybe;
    std::vector<Inner> items;
    bool flag = false;
  };
  const Outer o{"hi", std::nullopt, {{1}, {2}}, true};
  check(dump(to_json(o)) == R"({"s":"hi","items":[{"n":1},{"n":2}],"flag":true})", "to_json omits an empty optional");
  auto back = from_json<Outer>(to_json(o));
  check(back && back->s == "hi" && !back->maybe && back->items.size() == 2 && back->items[1].n == 2 && back->flag, "from_json");
  check(!from_json<Outer>(*parse(R"({"s":1,"items":[],"flag":true})")), "a member of the wrong type is an error");
  check(!from_json<Inner>(*parse(R"({"n":1.5})")), "an integer must be whole");
}

void uris() {
  check(path_of("file:///a/b%20c.cpp") == "/a/b c.cpp" && path_of("file:///a/b%2fc") == "/a/b/c", "percent-decoding");
  check(uri_of("/a/b c.cpp") == "file:///a/b%20c.cpp", "encoding");
  check(path_of(uri_of("/x/é.cpp")) == "/x/é.cpp", "round trip");
  check(!path_of("http://x/y") && path_of("file://localhost/z") == "/z" && !path_of("file://other/z"), "other schemes and hosts");
}

void messages() {
  const std::string err =
      "a.cpp:3:10: error: use of undeclared identifier 'gg'\n"
      "inc/h.hpp:1:2: warning: odd: really\n"
      "/abs/x.cpp: error: no main\n"
      "ycxx: error: cannot read 'z.cpp'\n"
      "some note\n"
      "c:d.cpp:x:1: error: odd name\n";
  const std::vector<Diagnostic> got = parse_messages(err, "/w");
  check(got.size() == 5, "five messages, a line that is none skipped");
  if (got.size() != 5) return;
  auto is = [](const Diagnostic& d, std::string_view path, std::uint32_t line, std::uint32_t column, int severity, std::string_view message) {
    return d.path == path && d.line == line && d.column == column && d.severity == severity && d.message == message;
  };
  check(is(got[0], "/w/a.cpp", 3, 10, 1, "use of undeclared identifier 'gg'"), "an error at a position, made absolute");
  check(is(got[1], "/w/inc/h.hpp", 1, 2, 2, "odd: really"), "a warning, the message keeping its colons");
  check(is(got[2], "/abs/x.cpp", 1, 1, 1, "no main"), "a file-level error at 1:1");
  check(is(got[3], "", 1, 1, 1, "cannot read 'z.cpp'"), "a message about no file");
  check(is(got[4], "/w/c:d.cpp:x:1", 1, 1, 1, "odd name"), "a name that is no position stays a name");
}

void session() {
  Script io;
  std::vector<std::pair<std::string, Buffers>> asked;
  // A fake compiler: an error on every line of the document that contains "bad", and in a.h's
  // includer when a.h's open text says "broken".
  Diagnose diagnose = [&](const std::string& path, const Buffers& open) {
    asked.emplace_back(path, open);
    std::vector<Diagnostic> out;
    const std::string& text = open.at(path);
    std::uint32_t line = 1;
    for (std::size_t at = 0; at < text.size(); ++line) {
      const std::size_t eol = std::min(text.find('\n', at), text.size());
      const std::string_view l(text.data() + at, eol - at);
      if (const auto col = l.find("bad"); col != std::string_view::npos) out.push_back({path, line, static_cast<std::uint32_t>(col + 1), 1, "bad here"});
      at = eol + 1;
    }
    if (path == "/w/main.cpp" && open.contains("/w/a.h") && open.at("/w/a.h").contains("broken")) out.push_back({path, 1, 1, 1, "from a.h"});
    out.push_back({"/w/elsewhere.h", 1, 1, 1, "not this document"});
    return out;
  };
  io.send(request(1, "initialize", Json(Object{{"capabilities", Json(Object{})}})));
  io.send(notification("initialized", Json(Object{})));
  io.send(notification("textDocument/didOpen",
                       Json(Object{{"textDocument", Json(Object{{"uri", "file:///w/main.cpp"}, {"languageId", "cpp"}, {"version", 1},
                                                                {"text", "int main() {\n  é bad;\n}\n"}})}})));
  const int code = serve(io, diagnose);
  check(code == 1, "an editor that leaves without shutdown: exit status 1");
  auto r = io.replies();
  check(r.size() == 2, std::format("an initialize result and one publish ({} messages)", r.size()));
  if (r.size() == 2) {
    const Json* caps = (*r[0]["result"])["capabilities"];
    check(caps && *(*(*caps)["textDocumentSync"])["change"]->number() == 1, "full-text sync");
    const Json& p = *r[1]["params"];
    check(*p["uri"]->string() == "file:///w/main.cpp" && *p["version"]->number() == 1, "the document and its version");
    const Array& d = *p["diagnostics"]->array();
    check(d.size() == 1, "only this document's diagnostics");
    if (d.size() == 1) {
      const Json& start = *(*d[0]["range"])["start"];
      check(*start["line"]->number() == 1 && *start["character"]->number() == 4, "0-based line, UTF-16 column (é is one unit, two bytes)");
    }
  }

  // Two documents; changes that arrive together are diagnosed once; a header's unsaved text
  // reaches its includer; close; shutdown and exit.
  Script io2;
  asked.clear();
  const auto open = [&](const char* uri, const char* text) {
    io2.send(notification("textDocument/didOpen", Json(Object{{"textDocument", Json(Object{{"uri", uri}, {"version", 1}, {"text", text}})}})));
  };
  io2.send(request(1, "initialize"));
  open("file:///w/main.cpp", "int main() {}\n");
  open("file:///w/a.h", "int x;\n");
  io2.send(notification("textDocument/didChange",
                        Json(Object{{"textDocument", Json(Object{{"uri", "file:///w/a.h"}, {"version", 2}})},
                                    {"contentChanges", Json(Array{Json(Object{{"text", "broken\n"}})})}})));
  io2.send(request(2, "textDocument/hover", Json(Object{})));
  io2.pause();
  io2.send(notification("textDocument/didClose", Json(Object{{"textDocument", Json(Object{{"uri", "file:///w/a.h"}})}})));
  io2.pause();
  io2.send(request(3, "shutdown"));
  io2.send(notification("exit", Json(nullptr)));
  check(serve(io2, diagnose) == 0, "exit after shutdown: status 0");
  // One diagnosis of both documents after the batch, then main.cpp alone after the close.
  check(asked.size() == 3 && asked[0].second.size() == 2 && asked[2].first == "/w/main.cpp" && asked[2].second.size() == 1,
        std::format("diagnosed once per batch ({} calls)", asked.size()));
  bool saw_header = false, saw_unknown = false, saw_cleared = false;
  for (const Json& m : io2.replies()) {
    if (m["error"] && *m["id"]->number() == 2) saw_unknown = *(*m["error"])["code"]->number() == -32601;
    if (const Json* p = m["params"]; p && p->object() && (*p)["uri"]) {
      const Array& d = *(*p)["diagnostics"]->array();
      if (*(*p)["uri"]->string() == "file:///w/main.cpp" && !d.empty() && *d[0]["message"]->string() == "from a.h") saw_header = true;
      if (*(*p)["uri"]->string() == "file:///w/a.h" && d.empty()) saw_cleared = true;
    }
  }
  check(saw_header, "a header's unsaved text changes its includer's diagnostics");
  check(saw_unknown, "an unknown request gets MethodNotFound");
  check(saw_cleared, "a closed document's diagnostics are cleared");
}

void framing() {
  Script io;
  check(write_message(io, Json(Object{{"a", 1}})), "write");
  auto r = io.replies();
  check(r.size() == 1 && *(*r[0]["a"]).number() == 1, "a framed message reads back");
  struct Raw final : Channel {
    std::string in;
    std::size_t at = 0;
    std::size_t read(std::span<char> b) override {
      const std::size_t n = std::min(b.size(), in.size() - at);
      std::copy_n(in.data() + at, n, b.data());
      at += n;
      return n;
    }
    bool pending() override { return at < in.size(); }
    bool write(std::string_view) override { return true; }
  };
  for (const char* bad : {"Content-Length: x\r\n\r\n", "Content-Type: a\r\n\r\n{}", "Content-Length: 10\r\n\r\n{}"}) {
    Raw raw;
    raw.in = bad;
    check(!read_message(raw), std::format("rejected header: {:?}", bad));
  }
}

}  // namespace

int main() {
  json();
  uris();
  messages();
  session();
  framing();
  return ycxx::unit::result("lsp");
}
