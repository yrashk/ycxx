// The interactive trace browser's model (#245, #635): its state, what each key and mouse event
// does to it, and the frames it draws for a screen of a given size. No terminal is involved:
// `:browse` runs it on one, and unit tests drive it with event sequences and compare its frames,
// without SGR, against goldens.
//
// - The tree: what each request asked of the engine, nested by demand, in columns (time, worker,
//   memory) whose headers sort it, with a pane on the right for the selected cell (its outcome,
//   times, worker, subtree, and its value when the kind can show one).
// - The timeline: one lane per pool worker over the trace's span, each slice colored by the
//   outcome of the deepest cell the worker was in; a cursor scrubs through time and Enter jumps to
//   the cell under it in the tree.
// - Filters: `/` by kind, key, `is:<outcome>` or `w:<worker>`; `w` cycles one worker, 0–9 picks
//   one, `a` shows all again.
// - The mouse: a click selects a row, its marker or a double click folds it, a click on a column
//   header sorts by it, on a tab switches the view, on a pane focuses it; the wheel scrolls the
//   pane under it; the divider between the panes drags.
export module ycxx.trace:view;

import std;
import :model;

export namespace ycxx::trace {

// A key: one byte, or an escape sequence as one string (`\x1b[A`).
struct Key {
  std::string seq;
  bool operator==(const Key&) const = default;
};
// A mouse report (SGR, `?1006h`): what happened, and where, 0-based.
struct Mouse {
  enum class Act : std::uint8_t { press, release, drag, move, wheel_up, wheel_down };
  Act act = Act::press;
  std::uint8_t button = 0;   // 0 left, 1 middle, 2 right
  std::size_t x = 0, y = 0;  // column and row
  bool shift = false, alt = false, ctrl = false;
  bool operator==(const Mouse&) const = default;
};
using Input = std::variant<Key, Mouse>;

// What a terminal sent, as one input. `\x1b[<0;12;5M` is a left press at column 11, row 4, and
// `\x1b[<0;12;5m` its release; anything else is a key.
Input parse_input(std::string_view seq) {
  // A legacy report (`ESC [ M` and three bytes, each 32 more than its value) from a terminal that
  // ignores 1006: read as one, so its bytes never act as keys.
  if (seq.size() == 6 && seq.starts_with("\x1b[M")) {
    const auto at = [&](std::size_t i) { return static_cast<std::uint32_t>(static_cast<unsigned char>(seq[i])); };
    if (at(3) < 32 || at(4) < 33 || at(5) < 33) return Key{std::string(seq)};
    const std::uint32_t b = at(3) - 32;
    const bool release = (b & 3) == 3 && !(b & 64);
    return parse_input(std::format("\x1b[<{};{};{}{}", release ? 0 : b, at(4) - 32, at(5) - 32, release ? 'm' : 'M'));
  }
  if (seq.starts_with("\x1b[<") && seq.size() > 4 && (seq.back() == 'M' || seq.back() == 'm')) {
    std::array<std::uint32_t, 3> n{};
    std::string_view rest = seq.substr(3, seq.size() - 4);
    std::size_t i = 0;
    for (auto part : rest | std::views::split(';')) {
      const std::string_view p(part.begin(), part.end());
      if (i == n.size() || std::from_chars(p.data(), p.data() + p.size(), n[i]).ec != std::errc{}) return Key{std::string(seq)};
      ++i;
    }
    if (i != n.size() || n[1] == 0 || n[2] == 0) return Key{std::string(seq)};
    const std::uint32_t b = n[0];
    // Buttons 8–11 (back, forward) and the horizontal wheel mean nothing here: a key that does
    // nothing, never a click or a vertical scroll.
    if (b & 128) return Key{std::string(seq)};
    if ((b & 64) && (b & 2)) return Key{std::string(seq)};
    Mouse m;
    m.x = n[1] - 1;
    m.y = n[2] - 1;
    m.shift = (b & 4) != 0;
    m.alt = (b & 8) != 0;
    m.ctrl = (b & 16) != 0;
    m.button = static_cast<std::uint8_t>(b & 3);
    if (b & 64) m.act = (b & 1) ? Mouse::Act::wheel_down : Mouse::Act::wheel_up;
    else if (seq.back() == 'm') m.act = Mouse::Act::release;
    else if (b & 32) m.act = m.button == 3 ? Mouse::Act::move : Mouse::Act::drag;
    else m.act = Mouse::Act::press;
    return m;
  }
  return Key{std::string(seq)};
}

// Display columns of a string with SGR sequences in it (a code point is one column).
std::size_t columns(std::string_view s) {
  std::size_t n = 0;
  for (std::size_t i = 0; i < s.size(); ++i) {
    if (s[i] == '\x1b') {
      while (i < s.size() && s[i] != 'm') ++i;
      continue;
    }
    if ((static_cast<unsigned char>(s[i]) & 0xc0) != 0x80) ++n;
  }
  return n;
}
// `s` cut or padded to exactly `width` columns, with attributes reset at the end.
std::string fit(std::string_view s, std::size_t width) {
  std::string out;
  std::size_t n = 0;
  for (std::size_t i = 0; i < s.size(); ++i) {
    if (s[i] == '\x1b') {
      const std::size_t j = s.find('m', i);
      out += s.substr(i, j == std::string_view::npos ? s.size() - i : j - i + 1);
      i = j == std::string_view::npos ? s.size() : j;
      continue;
    }
    const bool lead = (static_cast<unsigned char>(s[i]) & 0xc0) != 0x80;
    if (lead && n == width) break;
    if (lead) ++n;
    out += s[i];
  }
  out += "\x1b[0m";
  out.append(width - n, ' ');
  return out;
}
// What `s` (with SGR) shows from column `from` on, without its SGR.
std::string slice_from(std::string_view s, std::size_t from) {
  std::string out;
  std::size_t col = 0;
  for (std::size_t i = 0; i < s.size(); ++i) {
    if (s[i] == '\x1b') {
      const std::size_t j = s.find('m', i);
      i = j == std::string_view::npos ? s.size() : j;
      continue;
    }
    if ((static_cast<unsigned char>(s[i]) & 0xc0) != 0x80) ++col;
    if (col > from) out += s[i];
  }
  return out;
}
// `s` in at most `width` columns, unpadded: cut, it ends in `…`, so a reader sees there is more.
std::string ellipsize(std::string_view s, std::size_t width) {
  if (columns(s) <= width) return std::string(s);
  if (width == 0) return {};
  // Only blanks past the edge: nothing is lost.
  if (slice_from(s, width).find_first_not_of(' ') == std::string::npos) return fit(s, width);
  std::string out = fit(s, width - 1);
  // `fit` padded nothing (the text was longer) and ended in a reset: the mark goes after it.
  return out + "…";
}
// A line made of pieces, each with a priority: those that fit `width`, in their order, `sep`
// between them. A piece that does not fit is left out, lowest priority first, so the line keeps
// what matters most and never runs off the screen; the last piece left is cut with `…`.
struct Piece {
  std::string text;
  int priority = 0;  // higher stays longer
};
std::string pack(const std::vector<Piece>& pieces, std::size_t width, std::string_view sep = "  ") {
  std::vector<bool> keep(pieces.size(), true);
  const auto length = [&] {
    std::size_t n = 0, k = 0;
    for (std::size_t i = 0; i < pieces.size(); ++i)
      if (keep[i]) n += columns(pieces[i].text) + (k++ ? columns(sep) : 0);
    return std::pair(n, k);
  };
  while (length().first > width && length().second > 1) {
    std::size_t drop = pieces.size();
    for (std::size_t i = pieces.size(); i-- > 0;)
      if (keep[i] && (drop == pieces.size() || pieces[i].priority < pieces[drop].priority)) drop = i;
    keep[drop] = false;
  }
  std::string out;
  bool first = true;
  for (std::size_t i = 0; i < pieces.size(); ++i) {
    if (!keep[i]) continue;
    if (!first) out += sep;
    out += pieces[i].text;
    first = false;
  }
  return ellipsize(out, width);
}
// `left` and `right` on one line of `width`: the right part kept to its end, the left cut first.
std::string spread(std::string_view left, std::string_view right, std::size_t width) {
  const std::size_t r = columns(right);
  if (r >= width) return ellipsize(right, width);
  std::string l = ellipsize(left, width - r - (r ? 1 : 0));
  const std::size_t gap = width - columns(l) - r;
  return l + std::string(gap, ' ') + std::string(right);
}
// Plain text in at most `width` columns, cut in its path, so it ends as it did:
// `watching …/scratch/pg`.
std::string ellipsize_path(std::string_view s, std::size_t width) {
  const std::size_t n = columns(s);
  if (n <= width) return std::string(s);
  const std::size_t slash = s.find('/');
  const std::string_view head = s.substr(0, slash == std::string_view::npos ? 0 : slash);
  if (width <= columns(head) + 2) return ellipsize(s, width);
  // The last `keep` columns of the text, after the head and `…`.
  const std::size_t keep = width - columns(head) - 1;
  std::size_t at = s.size(), col = 0;
  while (at > 0 && col < keep) {
    --at;
    if ((static_cast<unsigned char>(s[at]) & 0xc0) != 0x80) ++col;
  }
  return std::string(head) + "…" + std::string(s.substr(at));
}
// A path under `home` as `~/…`: the title bar's paths are the user's own.
std::string home_relative(std::string_view text, std::string_view home) {
  std::string out(text);
  if (home.empty() || home == "/") return out;
  for (std::size_t at = out.find(home); at != std::string::npos; at = out.find(home, at + 1)) {
    const std::size_t end = at + home.size();
    const bool starts = at == 0 || out[at - 1] == ' ' || out[at - 1] == '(';
    const bool ends = end == out.size() || out[end] == '/' || out[end] == ')' || out[end] == ' ';
    if (starts && ends) out.replace(at, home.size(), "~");
  }
  return out;
}
// `s` right-aligned in `width` columns (cut on the left when longer).
std::string pad_left(std::string_view s, std::size_t width) {
  const std::size_t n = columns(s);
  if (n >= width) return fit(s, width);
  return std::string(width - n, ' ') + std::string(s);
}
// The text without its SGR sequences: a frame as a golden test holds it.
std::string strip_sgr(std::string_view s) {
  std::string out;
  for (std::size_t i = 0; i < s.size(); ++i) {
    if (s[i] == '\x1b' && i + 1 < s.size() && s[i + 1] == '[') {
      const std::size_t j = s.find('m', i);
      if (j != std::string_view::npos) {
        i = j;
        continue;
      }
    }
    out += s[i];
  }
  return out;
}
std::string paint(std::string_view params, std::string_view text) { return std::format("\x1b[{}m{}\x1b[0m", params, text); }
// `line` under attributes `params` (a bar's background), kept across the resets inside it.
std::string under(std::string_view params, std::string_view line) {
  std::string out = std::format("\x1b[{}m", params);
  const std::string again = std::format("\x1b[0;{}m", params);
  for (std::size_t i = 0; i < line.size(); ++i) {
    if (line.substr(i).starts_with("\x1b[0m")) {
      out += again;
      i += 3;
    } else {
      out += line[i];
    }
  }
  return out;
}
// The selected row: reverse video, kept across the resets inside it.
std::string selected(std::string_view line) {
  // Faint text in reverse video is hard to read: the selected row shows it plain.
  std::string out = under("7", line);
  for (std::size_t at = out.find("\x1b[2m"); at != std::string::npos; at = out.find("\x1b[2m", at)) out.erase(at, 4);
  return out;
}

// `s` (with SGR) in reverse video from column `from` up to `to`, kept across the resets inside.
std::string highlight(std::string_view s, std::size_t from, std::size_t to) {
  std::string out;
  std::size_t col = 0;
  bool on = false;
  for (std::size_t i = 0; i < s.size(); ++i) {
    if (s[i] == '\x1b') {
      const std::size_t j = s.find('m', i);
      out += s.substr(i, j == std::string_view::npos ? s.size() - i : j - i + 1);
      i = j == std::string_view::npos ? s.size() : j;
      if (on) out += "\x1b[7m";
      continue;
    }
    if ((static_cast<unsigned char>(s[i]) & 0xc0) != 0x80) {
      if (on && col == to) out += "\x1b[27m", on = false;
      if (!on && col == from && from < to) out += "\x1b[7m", on = true;
      ++col;
    }
    out += s[i];
  }
  if (on) out += "\x1b[27m";
  return out;
}
// Columns [from, to) of a plain line (a code point is a column).
std::string slice_columns(std::string_view s, std::size_t from, std::size_t to) {
  std::size_t col = 0, begin = s.size(), end = s.size();
  for (std::size_t i = 0; i < s.size(); ++i) {
    if ((static_cast<unsigned char>(s[i]) & 0xc0) == 0x80) continue;
    if (col == from) begin = i;
    if (col == to) {
      end = i;
      break;
    }
    ++col;
  }
  return begin < end ? std::string(s.substr(begin, end - begin)) : std::string();
}

// Base64 (RFC 4648, padded), for OSC 52.
std::string base64(std::string_view s) {
  static constexpr std::string_view digits = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
  std::string out;
  for (std::size_t i = 0; i < s.size(); i += 3) {
    const auto at = [&](std::size_t k) { return k < s.size() ? static_cast<std::uint32_t>(static_cast<unsigned char>(s[k])) : 0u; };
    const std::uint32_t n = at(i) << 16 | at(i + 1) << 8 | at(i + 2);
    out += digits[n >> 18 & 63];
    out += digits[n >> 12 & 63];
    out += i + 1 < s.size() ? digits[n >> 6 & 63] : '=';
    out += i + 2 < s.size() ? digits[n & 63] : '=';
  }
  return out;
}
// What puts `text` on the system clipboard through the terminal (OSC 52), over SSH too. Inside
// tmux also the passthrough form, which reaches the outer terminal when tmux allows it.
std::string osc52(std::string_view text, bool tmux) {
  const std::string b = base64(text);
  std::string out = std::format("\x1b]52;c;{}\x07", b);
  if (tmux) out += std::format("\x1bPtmux;\x1b\x1b]52;c;{}\x07\x1b\\", b);
  return out;
}

// Splits text into lines of at most `width` columns. A line breaks after a space, `/` or `,` in
// its second half when it has one, else mid-word; no character is dropped, so the pieces joined
// are the text again (a copy of wrapped lines is the line).
std::vector<std::string> wrap(std::string_view text, std::size_t width) {
  std::vector<std::string> out;
  width = std::max<std::size_t>(width, 1);
  for (auto part : text | std::views::split('\n')) {
    std::string_view line(part.begin(), part.end());
    do {
      std::size_t cut = 0, n = 0, soft = 0, soft_n = 0;
      while (cut < line.size() && n < width) {
        const char c = line[cut];
        ++cut;
        while (cut < line.size() && (static_cast<unsigned char>(line[cut]) & 0xc0) == 0x80) ++cut;
        ++n;
        if (c == ' ' || c == '/' || c == ',') soft = cut, soft_n = n;
      }
      if (cut < line.size() && soft > 0 && soft_n * 2 >= width) cut = soft;
      out.emplace_back(line.substr(0, cut));
      line.remove_prefix(cut);
    } while (!line.empty());
  }
  return out;
}

enum class View : std::uint8_t { tree, timeline, histogram };
// What the toggles' line shows, each with its key: a click on one does what the key does.
enum class Toggle : std::uint8_t { filter, worker, order, sort, reused, source };
// Which pane the keys move: the tree, or the pane on its right (after a click on it).
enum class Focus : std::uint8_t { tree, detail };
// The tree pane's columns. A click on a header sorts by it (`cell`: as they started); `worker`
// does not sort.
enum class Column : std::uint8_t { cell, time, worker, alloc, retained, source };

// Where a point on the screen is, as the last frame drew it.
struct Hit {
  enum class Area : std::uint8_t {
    none,
    header,   // the summary line
    tab,      // a view's tab: `index` is the `View`
    toggle,   // a toggle's chip: `index` is the `Toggle`
    status,   // the rest of the toggles' line
    column,   // a column header: `index` is the `Column`
    row,      // a tree row: `index` in `Tree::rows()`
    marker,   // a tree row's ▸/▾: `index` as for `row`
    divider,  // between the tree and the pane
    title,    // the pane's title line
    detail,   // a line of the pane: `index` among its lines, scrolling included
    lane,     // a timeline lane: `index` the lane, `at` the slice
    metric,   // a histogram metric's switch: `index` the `Metric`
    grouping, // a histogram grouping: `index` the `Grouping`
    scope,    // the histogram's scope: everything, or the selected cell's subtree
    order,    // a histogram column's header: `index` the `Metric` it sorts by
    bar,      // a histogram group: `index` among the groups
    footer,
  };
  Area area = Area::none;
  std::size_t index = 0;
  std::size_t at = 0;
  bool operator==(const Hit&) const = default;
};

// What the terminal does after an input.
struct Action {
  bool quit = false;
  std::optional<std::string> copy{};  // text for the clipboard (#636)
};

// One screen: each line exactly the screen's width (SGR aside), and where the terminal's cursor
// shows, if it does (the filter prompt).
struct Frame {
  std::vector<std::string> lines;
  std::optional<std::pair<std::size_t, std::size_t>> cursor;  // row, column
};

class Browser {
 public:
  explicit Browser(Tree& tree) : tree_(tree) {}

  void resize(std::size_t rows, std::size_t cols) {
    rows_ = std::max<std::size_t>(rows, 6);  // the bars, the column headers and a row
    cols_ = std::max<std::size_t>(cols, 20);
  }
  // A live source (`ctl trace`, `--watch`, a build being traced): the source named, the latest
  // request on top (`newest_first`), and following the newest activity (#939).
  void set_live(std::string source, bool newest_first = true) {
    const char* home = std::getenv("HOME");
    live_ = home_relative(source, home ? home : "");
    if (newest_first) tree_.set_newest_first(true);
    follow_ = true;
  }
  // The source has ended: the title bar says so (`what`). A view still following goes back to
  // the top, where a trace browsed after its build opens.
  void end_live(std::string what) {
    live_.reset();
    ended_ = std::move(what);
    if (follow_) cursor_ = top_ = time_ = 0, detail_scroll_ = 0;
    follow_ = false;
  }
  // Runs `pump` (more events into the tree): following, the newest activity is selected; else the
  // selected node stays selected (or the one it was folded into), unless the view is at the top,
  // where it follows what comes; false once the source has ended.
  bool pump(const std::function<bool()>& f) {
    const auto before = tree_.rows();
    std::optional<std::int32_t> held = cursor_ > 0 && cursor_ < before.size() ? std::optional(before[cursor_].node) : std::nullopt;
    // A selection being made holds its end where it is, even at the top.
    if (rows_sel_ && (visual_ || drag_ == Drag::rows)) held = rows_sel_->end;
    const std::uint64_t resets = tree_.resets();
    const bool more = f();
    if (tree_.resets() != resets) {
      // A new tree (a new run of the request): nothing held refers to it.
      clear_selection();
      drag_ = Drag::none;
      cursor_ = top_ = detail_scroll_ = 0;
      bar_ = bars_top_ = 0;
      held.reset();
    }
    if (held && static_cast<std::size_t>(*held) >= tree_.nodes().size()) held.reset();
    if (follow_ && !(rows_sel_ && (visual_ || drag_ == Drag::rows))) follow();
    else if (held) keep_selected(tree_.shown(*held));
    return more;
  }
  // Whether the view follows the newest activity (#939); `f` turns it back on.
  bool following() const noexcept { return follow_; }

  Action handle(const Input& in, std::chrono::milliseconds now = {}) {
    if (const Mouse* m = std::get_if<Mouse>(&in)) {
      if (m->act == Mouse::Act::press) notice_.clear();
      return mouse(*m, now);
    }
    const std::string& k = std::get<Key>(in).seq;
    if (k.empty()) return {};
    notice_.clear();
    drop_stale_selection();
    if (prompt_) {
      edit_filter(k);
      return {};
    }
    if (help_) {
      if (k == "j" || k == "\x1b[B" || k == "\x1bOB") ++help_scroll_;
      else if (k == "k" || k == "\x1b[A" || k == "\x1bOA") help_scroll_ = help_scroll_ > 0 ? help_scroll_ - 1 : 0;
      else if (k == "\x1b[6~" || k == " ") help_scroll_ += help_room();
      else if (k == "\x1b[5~") help_scroll_ = help_scroll_ > help_room() ? help_scroll_ - help_room() : 0;
      else help_ = false;
      return {};
    }
    if (k == "q" || k == "\x03") return Action{.quit = true};
    if (k == "f" && live_) {
      follow_ = true;
      follow();
      notice_ = "following the newest activity";
      return {};
    }
    // Any scroll or selection by the user stops following (#939).
    const auto before = std::tuple(cursor_, top_, detail_scroll_, lane_, time_, rows_sel_.has_value() || text_sel_.has_value());
    const auto stop_following = [&] {
      if (before != std::tuple(cursor_, top_, detail_scroll_, lane_, time_, rows_sel_.has_value() || text_sel_.has_value())) follow_ = false;
    };
    if (view_ == View::tree) {
      if (std::optional<Action> a = selection_key(k)) {
        stop_following();
        return *a;
      }
    }
    if (view_ == View::tree && pane_focused() && detail_key(k)) {
      stop_following();
      return {};
    }
    if (view_ == View::histogram && histogram_key(k)) return {};
    if (view_ == View::tree) tree_key(k);
    else if (view_ == View::timeline) timeline_key(k);
    // A filter, a worker or a fold puts the selection on a row it chose, not the user: those
    // keep following.
    if (k != "/" && !prompt_ && k != "\r" && k != " " && k != "e" && k != "E" && k != "w" && k != "a" && k != "m" &&
        !(k.size() == 1 && std::isdigit(static_cast<unsigned char>(k[0]))))
      stop_following();
    common_key(k);
    // V in the tree: the keys move its end.
    if (visual_ && rows_sel_)
      if (const auto n = selected_node()) rows_sel_->end = *n;
    return {};
  }
  // What the last copy was, for the footer.
  void note(std::string text) { notice_ = std::move(text); }
  const std::string& notice() const noexcept { return notice_; }

  // While a drag is held past the edge of the tree or the pane, it keeps scrolling: the terminal
  // calls `tick` this often, as terminals report the pointer only when it moves.
  bool ticking() const noexcept { return (drag_ == Drag::rows || drag_ == Drag::text) && past_edge_ && last_drag_; }
  void tick() {
    if (!ticking()) return;
    const auto before = std::tuple(cursor_, top_, detail_scroll_, rows_sel_ ? rows_sel_->end : -1, text_sel_ ? text_sel_->end : PanePoint{});
    drag_to(*last_drag_);
    // At the end of what it scrolls, ticking stops (a release the terminal never reported, too).
    if (before == std::tuple(cursor_, top_, detail_scroll_, rows_sel_ ? rows_sel_->end : -1, text_sel_ ? text_sel_->end : PanePoint{})) past_edge_ = false;
  }
  static constexpr std::chrono::milliseconds tick_every{60};

  Frame frame() {
    drop_stale_selection();
    Frame f;
    f.lines = header_lines();
    std::vector<std::string> body = view_ == View::tree ? tree_body() : view_ == View::timeline ? timeline_body() : histogram_body();
    body.resize(body_height());
    f.lines.append_range(body);
    f.lines.push_back(footer());
    if (help_) overlay_help(f.lines);
    // Nothing runs off the screen: a line cut ends in `…`.
    for (std::string& l : f.lines) l = fit(ellipsize(l, cols_), cols_);
    while (f.lines.size() < rows_) f.lines.push_back(fit("", cols_));
    f.lines.resize(rows_);
    if (prompt_) f.cursor = std::pair<std::size_t, std::size_t>{rows_ - 1, std::min(cols_ - 1, prompt_label.size() + columns(prompt_shown()))};
    return f;
  }

  // What is at column `x`, row `y` of the last frame.
  Hit hit(std::size_t x, std::size_t y) const {
    const std::size_t hh = header_height();
    if (y == 0) {
      for (const Tab& t : tabs()) {
        // A tab the narrow bar cut (its last column is `…`) is not there to click.
        const bool drawn = t.x1 <= (tabs().back().x1 <= cols_ ? cols_ : cols_ - 1);
        if (drawn && x >= t.x0 && x < t.x1) return {Hit::Area::tab, std::to_underlying(t.view)};
      }
      return {Hit::Area::header};
    }
    if (y + 1 < hh) return {Hit::Area::header};
    if (y + 1 == hh) {
      for (const Chip& c : chips())
        if (x >= c.x0 && x < c.x1) return {Hit::Area::toggle, std::to_underlying(c.toggle)};
      return {Hit::Area::status};
    }
    if (y >= hh + body_height()) return {Hit::Area::footer};
    const std::size_t by = y - hh;
    if (view_ == View::histogram) return histogram_hit(x, by);
    if (view_ == View::timeline) {
      const std::size_t lanes = static_cast<std::size_t>(tree_.summary().workers) + 1;
      if (by >= 1 && by - 1 < lanes && x >= lane_prefix) return {Hit::Area::lane, by - 1, std::min(x - lane_prefix, lane_columns() - 1)};
      return {};
    }
    const std::size_t lw = left_width();
    // The divider, and the blank column right of it: a target of two columns to grab.
    if (split() && (x == lw || x == lw + 1)) return {Hit::Area::divider};
    if (split() && x > lw) return by == 0 ? Hit{Hit::Area::title} : Hit{Hit::Area::detail, detail_scroll_ + by - 1};
    if (by == 0) {
      for (const Span& s : column_spans()) {
        if (x >= s.x0 && x < s.x1) return {Hit::Area::column, std::to_underlying(s.column)};
      }
      return {};
    }
    const auto rows = tree_.rows();
    const std::size_t row = top_ + by - 1;
    if (row >= rows.size()) return {};
    const std::size_t indent = 2 * rows[row].depth;
    // The marker, where it is drawn: a deep row's is cut off by the columns to its right.
    const auto spans = column_spans();
    const std::size_t cell_w = spans.size() > 1 && spans[0].x1 > 1 ? spans[0].x1 - 1 : spans[0].x1;
    if (x >= indent && x < indent + 2 && indent + 2 <= cell_w && !tree_.node(rows[row].node).children.empty())
      return {Hit::Area::marker, row};
    return {Hit::Area::row, row};
  }

  // For tests and the terminal.
  View view() const noexcept { return view_; }
  // The pane has the keys only while it is shown (a narrowed terminal hides it).
  Focus focus() const noexcept { return pane_focused() ? Focus::detail : Focus::tree; }
  std::size_t cursor() const noexcept { return cursor_; }
  std::size_t top() const noexcept { return top_; }
  std::size_t detail_scroll() const noexcept { return detail_scroll_; }
  std::size_t lane() const noexcept { return lane_; }
  std::size_t time() const noexcept { return time_; }
  bool dragging() const noexcept { return drag_ == Drag::divider; }
  std::size_t left_width() const {
    if (!split()) return cols_;
    return std::clamp(cols_ * split_ / 1000, min_pane, cols_ - min_pane);
  }
  // Rows above the body: the title bar, the summary (it wraps rather than run off a narrow
  // screen) and the toggles.
  std::size_t header_height() const { return header_lines().size(); }

  // Two presses on one row this close together are a double click.
  static constexpr std::chrono::milliseconds double_click{400};
  // Rows the wheel moves.
  static constexpr std::size_t wheel_step = 3;

 private:
  static constexpr std::size_t min_pane = 24;
  static constexpr std::size_t resize_step = 4;  // columns `<` and `>` move the divider
  static constexpr std::size_t lane_prefix = 9;  // `▶    w0 │`

  // ---- the mouse
  Action mouse(const Mouse& m, std::chrono::milliseconds now) {
    if (m.act == Mouse::Act::move) return {};
    // A click on a row, the pane or the timeline, the wheel, or a drag: the user's own view, which
    // following would move away from (#939).
    if (m.act == Mouse::Act::drag || m.act == Mouse::Act::wheel_up || m.act == Mouse::Act::wheel_down) follow_ = false;
    if (m.act == Mouse::Act::press && !help_) {
      const Hit::Area a = hit(m.x, m.y).area;
      if (a == Hit::Area::row || a == Hit::Area::marker || a == Hit::Area::detail || a == Hit::Area::lane) follow_ = false;
    }
    if (m.act == Mouse::Act::drag) {
      drag_to(m);
      return {};
    }
    if (m.act == Mouse::Act::release) return end_drag();
    if (help_) {
      if (m.act == Mouse::Act::press) help_ = false;
      return {};
    }
    const Hit h = hit(m.x, m.y);
    if (m.act == Mouse::Act::wheel_up || m.act == Mouse::Act::wheel_down) {
      wheel(h, m.act == Mouse::Act::wheel_down ? 1 : -1, m.shift);
      return {};
    }
    if (m.button != 0) return {};
    // A double click on a bar: its first press opened the tree, and the second lands on whatever
    // row is there now; it is no click.
    if (now - opened_at_ <= double_click) return {};
    // Twice on the same thing (row, slice), not merely the same line.
    const std::int32_t node = row_node(h);
    const bool twice = last_press_ && now - last_press_->time <= double_click && last_press_->hit == h && last_press_->node == node;
    last_press_ = twice ? std::nullopt : std::optional(Press{h, now, node});
    // Every press starts afresh: a drag whose release was lost ends here, and so does a selection.
    drag_ = Drag::none;
    moved_ = false;
    clear_selection();
    if (prompt_) prompt_ = false;  // a click elsewhere keeps what was typed
    switch (h.area) {
      case Hit::Area::tab:
        view_ = static_cast<View>(h.index);
        break;
      case Hit::Area::column:
        sort_by(static_cast<Column>(h.index));
        break;
      case Hit::Area::toggle:
        toggle(static_cast<Toggle>(h.index));
        break;
      case Hit::Area::row:
        focus_ = Focus::tree;
        if (cursor_ != h.index) detail_scroll_ = 0;
        cursor_ = h.index;
        if (twice) tree_.toggle(tree_.rows()[cursor_].node);
        else rows_sel_ = RowSel{node, node}, drag_ = Drag::rows;  // a drag selects rows
        break;
      case Hit::Area::marker:
        focus_ = Focus::tree;
        if (cursor_ != h.index) detail_scroll_ = 0;
        cursor_ = h.index;
        tree_.toggle(tree_.rows()[cursor_].node);
        last_press_.reset();  // a second click on the marker folds it back, not twice
        break;
      case Hit::Area::divider:
        drag_ = Drag::divider;
        break;
      case Hit::Area::title:
        focus_ = Focus::detail;
        break;
      case Hit::Area::detail: {
        focus_ = Focus::detail;
        const PanePoint p = pane_point(m.x, m.y, false);
        text_sel_ = TextSel{p, p, false, selected_node().value_or(-1), pane_width()}, drag_ = Drag::text;  // a drag selects text
        break;
      }
      case Hit::Area::lane:
        lane_ = h.index;
        time_ = h.at;
        if (twice) jump_to_slice();
        break;
      case Hit::Area::metric:
        toggle_metric(static_cast<Metric>(h.index));
        break;
      case Hit::Area::grouping:
        group_by_ = static_cast<Grouping>(h.index), bar_ = bars_top_ = 0;
        break;
      case Hit::Area::scope:
        within_ = !within_, bar_ = bars_top_ = 0;
        break;
      case Hit::Area::order:
        sort_by_ = static_cast<Metric>(h.index);
        break;
      case Hit::Area::bar:
        bar_ = h.index;
        open_bar();  // a click on a bar shows its cells in the tree
        opened_at_ = now;
        break;
      default:
        break;
    }
    return {};
  }
  // The pointer moved with the button down: the divider, or the selection's end, follows it; past
  // the edge of a list the list scrolls.
  void drag_to(const Mouse& m) {
    last_drag_ = m;
    past_edge_ = false;
    if (drag_ == Drag::divider) {
      if (split()) place_divider(m.x);
    } else if (drag_ == Drag::rows && rows_sel_) {
      const auto rows = tree_.rows();
      const std::size_t n = rows.size(), first = header_height() + 1, hgt = list_height();
      if (n == 0) return;
      std::size_t row = 0;
      past_edge_ = m.y < first || m.y - first >= hgt;
      if (m.y < first) row = top_ > 0 ? top_ - 1 : 0;
      else if (m.y - first >= hgt) row = std::min(top_ + hgt, n - 1);
      else row = std::min(top_ + (m.y - first), n - 1);
      moved_ = moved_ || rows[row].node != rows_sel_->anchor;
      cursor_ = row;
      // The view follows at once, so the next tick goes a row further.
      if (cursor_ < top_) top_ = cursor_;
      if (cursor_ >= top_ + hgt) top_ = cursor_ - hgt + 1;
      rows_sel_->end = rows[row].node;
    } else if (drag_ == Drag::text && text_sel_) {
      const std::size_t first = header_height() + 1;
      past_edge_ = m.y < first || m.y - first >= pane_shown(pane_lines().size());
      const PanePoint p = pane_point(m.x, m.y, true);
      moved_ = moved_ || p != text_sel_->anchor;
      text_sel_->end = p;
    }
  }
  // The button came up: what a drag selected is copied, as a terminal's own selection is.
  Action end_drag() {
    const Drag d = std::exchange(drag_, Drag::none);
    if (!moved_) {
      if (d == Drag::rows) rows_sel_.reset();
      if (d == Drag::text) text_sel_.reset();
      return {};
    }
    if (d == Drag::rows && rows_sel_) return copy(rows_text(*rows_sel_), rows_count(*rows_sel_));
    if (d == Drag::text && text_sel_) return copy(selection_text(*text_sel_), "the selection");
    return {};
  }
  void wheel(const Hit& h, int dir, bool fast) {
    const std::size_t step = fast ? wheel_step * 4 : wheel_step;
    if (view_ == View::histogram) {
      // The selected bar moves with the view, as the tree's selection does.
      const std::size_t n = bars().size();
      bars_top_ = dir > 0 ? std::min(bars_top_ + step, n > bar_rows() ? n - bar_rows() : 0) : (bars_top_ > step ? bars_top_ - step : 0);
      if (n > 0) bar_ = std::clamp(bar_, bars_top_, std::min(bars_top_ + bar_rows(), n) - 1);
      return;
    }
    if (view_ == View::timeline) {
      const std::size_t by = fast ? 10 : 1;
      time_ = dir > 0 ? std::min(time_ + by, lane_columns() - 1) : (time_ > by ? time_ - by : 0);
      return;
    }
    if (h.area == Hit::Area::detail || h.area == Hit::Area::title) {
      detail_scroll_ = dir > 0 ? detail_scroll_ + step : (detail_scroll_ > step ? detail_scroll_ - step : 0);
      return;
    }
    if (h.area != Hit::Area::row && h.area != Hit::Area::marker && h.area != Hit::Area::column && h.area != Hit::Area::none) return;
    // The tree: the view moves, and the selection stays in it.
    const std::size_t n = tree_.rows().size(), hgt = list_height();
    const std::size_t last_top = n > hgt ? n - hgt : 0;
    top_ = dir > 0 ? std::min(top_ + step, last_top) : (top_ > step ? top_ - step : 0);
    if (n > 0) cursor_ = std::clamp(cursor_, top_, std::min(top_ + hgt, n) - 1);
    detail_scroll_ = 0;
    // A selection being made follows the cursor the wheel moved.
    if (rows_sel_ && (visual_ || drag_ == Drag::rows))
      if (const auto s = selected_node()) rows_sel_->end = *s;
  }
  // The divider at column `x`, kept as a share of the width, so a resized terminal keeps it.
  void place_divider(std::size_t x) {
    x = std::clamp(x, min_pane, cols_ - min_pane);
    split_ = (x * 1000 + cols_ - 1) / cols_;
  }
  void sort_by(Column c) {
    const auto order = [&]() -> std::optional<Tree::Order> {
      switch (c) {
        case Column::cell: return Tree::Order::start;
        case Column::time: return Tree::Order::time;
        case Column::alloc: return Tree::Order::alloc;
        case Column::retained: return Tree::Order::retained;
        case Column::worker:
        case Column::source: return std::nullopt;
      }
      return std::nullopt;
    }();
    if (!order) return;
    // A second click on the sorted column goes back to start order.
    reorder(tree_.order() == *order ? Tree::Order::start : *order);
  }
  void reorder(Tree::Order o) {
    clear_selection();  // rows between two cells are others in another order
    const std::optional<std::int32_t> held = selected_node();
    tree_.set_order(o);
    if (held) keep_selected(*held);
  }

  // ---- selection and copy (#636)
  // A point in the pane: a line of it (counting what is scrolled away) and a column.
  struct PanePoint {
    std::size_t line = 0, col = 0;
    auto operator<=>(const PanePoint&) const = default;
  };
  // Rows from one node to another, as shown: nodes, so a live feed adding rows keeps it in place.
  struct RowSel {
    std::int32_t anchor, end;
  };
  // Text of one node's pane at one width: its line numbers mean nothing for another node or width.
  struct TextSel {
    PanePoint anchor, end;
    bool lines = false;  // whole lines (V)
    std::int32_t node = -1;
    std::size_t width = 0;
  };
  // A line of the pane; `cont`: it continues the line above, wrapped.
  struct PaneLine {
    std::string text;
    bool cont = false;
  };
  enum class Drag : std::uint8_t { none, divider, rows, text };

  void clear_selection() {
    rows_sel_.reset();
    text_sel_.reset();
    visual_ = false;
    past_edge_ = false;
  }
  // A selection whose rows are no longer shown, or whose pane is another node's or width's, goes.
  void drop_stale_selection() {
    if (rows_sel_ && !row_range(*rows_sel_)) clear_selection();
    if (text_sel_ && (text_sel_->node != selected_node().value_or(-1) || text_sel_->width != pane_width() || !split())) clear_selection();
  }
  Action copy(std::string text, std::string_view what) {
    clear_selection();
    if (text.empty()) {
      notice_ = "nothing to copy";
      return {};
    }
    notice_ = std::format("copied {}", what);
    Action a;
    a.copy = std::move(text);
    return a;
  }
  // The rows a selection spans now, first and last; none when one of its ends is not shown.
  std::optional<std::pair<std::size_t, std::size_t>> row_range(const RowSel& r) const {
    const auto rows = tree_.rows();
    std::optional<std::size_t> a, b;
    for (std::size_t i = 0; i < rows.size(); ++i) {
      if (rows[i].node == r.anchor) a = i;
      if (rows[i].node == r.end) b = i;
    }
    if (!a || !b) return std::nullopt;
    return std::minmax(*a, *b);
  }
  std::string rows_count(const RowSel& r) const {
    const auto range = row_range(r);
    const std::size_t n = range ? range->second - range->first + 1 : 0;
    return std::format("{} row{}", n, n == 1 ? "" : "s");
  }
  // The pane's lines for the selected node, at the pane's width.
  std::vector<PaneLine> pane_lines() const {
    const auto rows = tree_.rows();
    if (rows.empty()) return {};
    return detail(tree_.node(rows[std::min(cursor_, rows.size() - 1)].node), pane_width());
  }
  std::size_t pane_width() const { return cols_ > left_width() + 2 ? cols_ - left_width() - 2 : 1; }
  // Lines of the pane on screen (its last row says where it is when it is longer).
  std::size_t pane_shown(std::size_t lines) const {
    const std::size_t hgt = list_height();
    return lines > hgt && hgt > 1 ? hgt - 1 : hgt;
  }
  // The pane's point under the pointer; above or below the pane, while dragging (`scroll`), the
  // pane scrolls a line.
  PanePoint pane_point(std::size_t x, std::size_t y, bool scroll) {
    const std::size_t lines = pane_lines().size(), first = header_height() + 1, shown = pane_shown(lines);
    const std::size_t lw = left_width();
    PanePoint p;
    p.col = x > lw ? x - lw - 1 : 0;
    if (y < first) {
      if (scroll && detail_scroll_ > 0) --detail_scroll_;
      p.line = detail_scroll_;
    } else if (y - first >= shown) {
      if (scroll && detail_scroll_ + shown < lines) ++detail_scroll_;
      p.line = detail_scroll_ + shown - 1;
    } else {
      p.line = detail_scroll_ + (y - first);
    }
    if (lines > 0) p.line = std::min(p.line, lines - 1);
    return p;
  }

  // Keys that select and copy, in the tree view: V selects (rows in the tree, lines in the pane),
  // y copies the selection or the node, Y the node's subtree, Escape drops a selection.
  std::optional<Action> selection_key(const std::string& k) {
    const auto rows = tree_.rows();
    if (k == "V") {
      if (visual_) {
        clear_selection();
      } else if (pane_focused()) {
        text_sel_ = TextSel{{detail_scroll_, 0}, {detail_scroll_, 0}, true, selected_node().value_or(-1), pane_width()};
        visual_ = true;
      } else if (!rows.empty()) {
        rows_sel_ = RowSel{rows[cursor_].node, rows[cursor_].node};
        visual_ = true;
      }
      return Action{};
    }
    if (k == "y") {
      if (rows_sel_) return copy(rows_text(*rows_sel_), rows_count(*rows_sel_));
      if (text_sel_) return copy(selection_text(*text_sel_), text_sel_->lines ? "the lines" : "the selection");
      if (rows.empty()) return Action{};
      return copy(node_text(tree_.node(rows[cursor_].node)), "the cell");
    }
    if (k == "p") {
      // Where the cell is in the source, as an editor opens it (#638).
      if (rows.empty()) return Action{};
      const std::string where = span_start(tree_.node(rows[cursor_].node).ev.span);
      if (where.empty()) {
        notice_ = "no source for this cell";
        return Action{};
      }
      return copy(where, where);
    }
    if (k == "S") {
      show_source_ = !show_source_;
      return Action{};
    }
    if (k == "Y") {
      if (rows.empty()) return Action{};
      const std::int32_t n = rows[cursor_].node;
      return copy(subtree_text(n), std::format("the subtree ({} cells)", tree_.node(n).subtree));
    }
    if (k == "\x1b" && (rows_sel_ || text_sel_)) {
      clear_selection();
      return Action{};
    }
    // V in the pane: the movement keys move the selection's end, and the pane follows it.
    if (visual_ && text_sel_ && pane_focused()) {
      const std::size_t lines = pane_lines().size();
      std::size_t& end = text_sel_->end.line;
      if (k == "j" || k == "\x1b[B" || k == "\x1bOB") end = lines > 0 ? std::min(end + 1, lines - 1) : 0;
      else if (k == "k" || k == "\x1b[A" || k == "\x1bOA") end = end > 0 ? end - 1 : 0;
      else return std::nullopt;
      const std::size_t shown = pane_shown(lines);
      if (end < detail_scroll_) detail_scroll_ = end;
      if (end >= detail_scroll_ + shown) detail_scroll_ = end - shown + 1;
      return Action{};
    }
    // Any other key leaves a mouse selection behind (it was copied when the button came up).
    if (!visual_) clear_selection();
    return std::nullopt;
  }

  // A row as the stream prints it (times, worker, memory summed over its subtree), indented `depth`.
  std::string row_line(std::int32_t node, std::uint32_t depth) const {
    const Tree::Node& n = tree_.node(node);
    Event e = n.ev;
    e.depth = depth;
    const auto at = tree_.error_origin_below(node);
    const std::string below(first_line(at ? tree_.node(*at).ev.errors : e.errors));
    return stream_line(e, Style{false, true}, tree_.error_here(node), below, n.incl);
  }
  // The rows between a selection's ends, as shown (folded rows stay folded), indented from the
  // shallowest.
  std::string rows_text(const RowSel& r) const {
    const auto rows = tree_.rows();
    const auto range = row_range(r);
    if (!range) return {};
    const auto [lo, hi] = *range;
    std::uint32_t base = rows[lo].depth;
    for (std::size_t i = lo; i <= hi; ++i) base = std::min(base, rows[i].depth);
    std::string out;
    for (std::size_t i = lo; i <= hi; ++i) out += row_line(rows[i].node, rows[i].depth - base) + '\n';
    return out;
  }
  // A node and everything under it, folded or filtered away or not, in start order.
  std::string subtree_text(std::int32_t node) const {
    std::string out;
    const auto walk = [&](this const auto& self, std::int32_t i, std::uint32_t depth) -> void {
      out += row_line(i, depth) + '\n';
      for (std::int32_t c : tree_.node(i).children) self(c, depth + 1);
    };
    walk(node, 0);
    return out;
  }
  // What the pane shows for a node, unwrapped and plain.
  std::string node_text(const Tree::Node& n) const {
    std::string out;
    for (const PaneLine& l : detail(n, std::numeric_limits<std::uint32_t>::max())) {
      std::string p = strip_sgr(l.text);
      if (p.starts_with(' ')) p.erase(0, 1);  // the pane's margin
      out += p + '\n';
    }
    while (out.ends_with("\n\n")) out.pop_back();
    return out;
  }
  // The columns of pane line `line` a selection covers, [from, to); column 0 is the margin.
  static std::pair<std::size_t, std::size_t> covered(const TextSel& t, std::size_t line, std::size_t width) {
    const auto [lo, hi] = std::minmax(t.anchor, t.end);
    if (line < lo.line || line > hi.line) return {0, 0};
    if (t.lines) return {1, width};
    const std::size_t from = line == lo.line ? std::max<std::size_t>(lo.col, 1) : 1;
    const std::size_t to = line == hi.line ? std::min(hi.col + 1, width) : width;
    return {from, std::max(from, to)};
  }
  std::string selection_text(const TextSel& t) const {
    const std::vector<PaneLine> lines = pane_lines();
    const auto [lo, hi] = std::minmax(t.anchor, t.end);
    std::string out;
    for (std::size_t i = lo.line; i <= hi.line && i < lines.size(); ++i) {
      const std::string plain = strip_sgr(lines[i].text);
      const auto [from, to] = covered(t, i, columns(plain));
      if (i > lo.line && !lines[i].cont) out += '\n';
      out += slice_columns(plain, from, to);
    }
    if (t.lines) out += '\n';
    return out;
  }

  // ---- keys
  // The pane, focused: the movement keys scroll it; h, ← or Escape give the keys back to the tree.
  bool detail_key(const std::string& k) {
    const std::size_t page = list_height() - 1;
    if (k == "j" || k == "\x1b[B" || k == "\x1bOB") ++detail_scroll_;
    else if (k == "k" || k == "\x1b[A" || k == "\x1bOA") detail_scroll_ = detail_scroll_ > 0 ? detail_scroll_ - 1 : 0;
    else if (k == "\x1b[6~" || k == "\x06") detail_scroll_ += page;
    else if (k == "\x1b[5~" || k == "\x02") detail_scroll_ = detail_scroll_ > page ? detail_scroll_ - page : 0;
    else if (k == "g" || k == "\x1b[H") detail_scroll_ = 0;
    else if (k == "G" || k == "\x1b[F") detail_scroll_ = std::numeric_limits<std::size_t>::max() / 2;
    else if (k == "h" || k == "\x1b[D" || k == "\x1bOD" || k == "\x1b") focus_ = Focus::tree, clear_selection();
    else return false;
    return true;
  }
  void common_key(const std::string& k) {
    // t and Tab: the tree and the timeline, as ever; b: the histogram and back.
    if (k == "t") view_ = view_ == View::tree ? View::timeline : View::tree, clear_selection();
    else if (k == "\t") view_ = view_ == View::tree ? View::timeline : View::tree, clear_selection();  // as ever
    else if (k == "b") view_ = view_ == View::histogram ? View::tree : View::histogram, clear_selection();
    else if (k == "/") prompt_ = true, typed_ = filter_;
    else if (k == "?") help_ = true, help_scroll_ = 0;
    else if (k == "v") detail_ = !detail_, focus_ = Focus::tree;
    // The pane's width: < moves the divider left (a wider pane), > right.
    else if ((k == "<" || k == ">") && split()) place_divider(k == "<" ? (left_width() > resize_step ? left_width() - resize_step : 0) : left_width() + resize_step);
    // The pane: J/K a line, ]/[ a page, }/{ its end and top (clamped when drawn).
    else if (k == "J") ++detail_scroll_;
    else if (k == "K") detail_scroll_ = detail_scroll_ > 0 ? detail_scroll_ - 1 : 0;
    else if (k == "]") detail_scroll_ += list_height() - 1;
    else if (k == "[") detail_scroll_ = detail_scroll_ > list_height() - 1 ? detail_scroll_ - (list_height() - 1) : 0;
    else if (k == "}") detail_scroll_ = std::numeric_limits<std::size_t>::max() / 2;
    else if (k == "{") detail_scroll_ = 0;
    else if (k == "w") cycle_worker();
    else if (k == "a") pick_worker(std::nullopt);
    else if (k.size() == 1 && std::isdigit(static_cast<unsigned char>(k[0]))) pick_worker(k[0] - '0');
    else if (k == "m") pick_worker(-1);
    else if (k == "r") {
      clear_selection();
      const std::optional<std::int32_t> held = selected_node();
      tree_.set_hide_reused(!tree_.hide_reused());
      if (held) keep_selected(*held);
    } else if (k == "s") {
      // Start order, then most allocated in the subtree, most retained, longest first (#274).
      reorder(static_cast<Tree::Order>((std::to_underlying(tree_.order()) + 1) % Tree::order_count));
    } else if (k == "o") {
      clear_selection();
      const std::optional<std::int32_t> held = selected_node();
      tree_.set_newest_first(!tree_.newest_first());
      if (held) keep_selected(*held);
    }
  }
  void tree_key(const std::string& k) {
    const auto rows = tree_.rows();
    const std::size_t page = list_height();
    if (k == "j" || k == "\x1b[B" || k == "\x1bOB") move(1, rows.size());
    else if (k == "k" || k == "\x1b[A" || k == "\x1bOA") move(-1, rows.size());
    else if (k == "\x1b[6~" || k == "\x06") move(static_cast<long>(page), rows.size());
    else if (k == "\x1b[5~" || k == "\x02") move(-static_cast<long>(page), rows.size());
    else if (k == "g" || k == "\x1b[H") cursor_ = 0;
    else if (k == "G" || k == "\x1b[F") cursor_ = rows.empty() ? 0 : rows.size() - 1;
    else if (rows.empty()) return;
    if (k == "\r" || k == " " || k == "l" || k == "\x1b[C" || k == "\x1bOC" || k == "h" || k == "\x1b[D" || k == "\x1bOD" || k == "e" || k == "E")
      clear_selection();  // folding moves rows between a selection's ends
    if (k == "\r" || k == " ") tree_.toggle(rows[cursor_].node);
    else if (k == "l" || k == "\x1b[C" || k == "\x1bOC") tree_.set_expanded(rows[cursor_].node, true);
    else if (k == "h" || k == "\x1b[D" || k == "\x1bOD") collapse_or_parent(rows);
    else if (k == "e") tree_.expand_all(true);
    else if (k == "E") tree_.expand_all(false), cursor_ = 0;
    else if (k == "n") next_execution(rows);
    else if (k == "x") next_error(rows);
    if (k != "J" && k != "K" && k != "]" && k != "[" && k != "}" && k != "{") detail_scroll_ = 0;
  }
  void timeline_key(const std::string& k) {
    const std::size_t lanes = static_cast<std::size_t>(tree_.summary().workers) + 1;
    const std::size_t cols = lane_columns();
    if (k == "j" || k == "\x1b[B" || k == "\x1bOB") lane_ = std::min(lane_ + 1, lanes - 1);
    else if (k == "k" || k == "\x1b[A" || k == "\x1bOA") lane_ = lane_ > 0 ? lane_ - 1 : 0;
    else if (k == "l" || k == "\x1b[C" || k == "\x1bOC") time_ = std::min(time_ + 1, cols - 1);
    else if (k == "h" || k == "\x1b[D" || k == "\x1bOD") time_ = time_ > 0 ? time_ - 1 : 0;
    else if (k == "L") time_ = std::min(time_ + 10, cols - 1);
    else if (k == "H") time_ = time_ > 10 ? time_ - 10 : 0;
    else if (k == "g") time_ = 0;
    else if (k == "G") time_ = cols - 1;
    else if (k == "\r" || k == " ") jump_to_slice();
  }
  void edit_filter(const std::string& k) {
    if (k == "\r") {
      prompt_ = false;
    } else if (k == "\x1b" || k == "\x03") {
      prompt_ = false;
      typed_ = filter_;
    } else if (k == "\x7f" || k == "\b") {
      if (!typed_.empty()) typed_.pop_back();
    } else if (k.size() == 1 && std::isprint(static_cast<unsigned char>(k[0]))) {
      typed_ += k;
    }
    // Live: the view follows each keystroke.
    clear_selection();
    filter_ = typed_;
    tree_.set_filter(filter_);
    cursor_ = 0;
  }

  std::optional<std::int32_t> selected_node() const {
    const auto rows = tree_.rows();
    return rows.empty() ? std::nullopt : std::optional(rows[std::min(cursor_, rows.size() - 1)].node);
  }
  void move(long by, std::size_t n) {
    if (n == 0) return;
    const long to = std::clamp(static_cast<long>(cursor_) + by, 0L, static_cast<long>(n) - 1);
    cursor_ = static_cast<std::size_t>(to);
  }
  void collapse_or_parent(const std::vector<Tree::Row>& rows) {
    const Tree::Node& n = tree_.node(rows[cursor_].node);
    if (n.expanded) {
      tree_.set_expanded(rows[cursor_].node, false);
      return;
    }
    for (std::size_t i = cursor_; i-- > 0;) {
      if (rows[i].node == n.parent) {
        cursor_ = i;
        return;
      }
    }
  }
  // The next cell that ran (executed or cut off), in the tree's order, opening the tree to it.
  void next_execution(const std::vector<Tree::Row>& rows) {
    const std::vector<std::int32_t> order = tree_.preorder();
    const auto from = std::ranges::find(order, rows[cursor_].node);
    for (auto it = from == order.end() ? order.end() : from + 1; it != order.end(); ++it) {
      const Event& e = tree_.node(*it).ev;
      if (ran(e) && tree_.matches(e)) {
        select(*it);
        return;
      }
    }
  }
  // Following (#939): the newest activity selected, and in view. That is the cell started last
  // that still runs (the newest cell, once none runs), or the nearest of its ancestors the tree
  // shows; in the timeline, the cursor at the end. The histogram's bars hang on the selection
  // (`i`, the children grouping): there it stays put.
  void follow() {
    if (view_ == View::histogram) return;
    time_ = lane_columns() - 1;
    const std::optional<std::int32_t> newest = tree_.newest();
    if (!newest) return;
    const auto rows = tree_.rows();
    std::unordered_map<std::int32_t, std::size_t> at;
    for (std::size_t i = 0; i < rows.size(); ++i) at.emplace(rows[i].node, i);
    for (std::int32_t n = tree_.shown(*newest); n >= 0; n = tree_.node(n).parent) {
      if (const auto it = at.find(n); it != at.end()) {
        if (cursor_ != it->second) detail_scroll_ = 0;
        cursor_ = it->second;
        // The view scrolls to it as it draws (`tree_body`).
        return;
      }
    }
  }
  // The row of `node`, if shown, becomes the selection.
  void keep_selected(std::int32_t node) {
    const auto rows = tree_.rows();
    for (std::size_t i = 0; i < rows.size(); ++i) {
      if (rows[i].node == node) cursor_ = i;
    }
  }
  // The next node where errors start, after the selection (wrapping), opening the tree to it.
  void next_error(const std::vector<Tree::Row>& rows) {
    const std::vector<std::int32_t> order = tree_.preorder();
    const auto from = static_cast<std::size_t>(std::ranges::find(order, rows[cursor_].node) - order.begin());
    for (std::size_t k = 1; k <= order.size(); ++k) {
      const std::int32_t i = order[(from + k) % order.size()];
      if (tree_.error_here(i) && tree_.matches(tree_.node(i).ev)) {
        select(i);
        return;
      }
    }
  }
  // The user's choice of a node (n, x, a slice or a bar): following stops.
  void select(std::int32_t node) {
    follow_ = false;
    tree_.reveal(node);
    keep_selected(node);
  }
  void cycle_worker() {
    const std::int32_t workers = tree_.summary().workers;
    const auto w = tree_.worker();
    if (!w) pick_worker(-1);
    else if (*w + 1 < workers) pick_worker(*w + 1);
    else pick_worker(std::nullopt);
  }
  void pick_worker(std::optional<std::int32_t> w) {
    clear_selection();
    tree_.set_worker(w);
    cursor_ = 0;
    if (w) lane_ = Timeline::lane_of(*w);
  }
  void jump_to_slice() {
    const Timeline tl = timeline(tree_, lane_columns());
    if (lane_ >= tl.lanes.size() || time_ >= tl.lanes[lane_].size()) return;  // the terminal shrank
    const std::int32_t node = tl.lanes[lane_][time_];
    if (node < 0) return;
    view_ = View::tree;
    focus_ = Focus::tree;
    select(node);
  }

  // ---- layout
  // The body: what the title bar, the summary, the toggles and the footer leave.
  std::size_t body_height() const {
    const std::size_t used = header_height() + 1;  // and the footer
    return rows_ > used + 1 ? rows_ - used : 1;
  }
  // Tree rows: the body less its line of column headers.
  std::size_t list_height() const { return body_height() > 2 ? body_height() - 1 : 1; }
  bool split() const { return detail_ && cols_ >= 100; }
  std::size_t lane_columns() const { return cols_ > 12 ? cols_ - lane_prefix : 1; }

  struct Tab {
    View view;
    std::string_view name;
    std::size_t x0, x1;
  };
  // The title bar's tabs, after its name: each padded by a column on either side.
  static constexpr std::string_view title_name = " ycxx trace ";
  std::vector<Tab> tabs() const {
    std::vector<Tab> out;
    std::size_t x = title_name.size() + 1;
    for (const auto& [v, name] : {std::pair{View::tree, std::string_view("tree")}, std::pair{View::timeline, std::string_view("timeline")},
                                  std::pair{View::histogram, std::string_view("histogram")}}) {
      out.push_back({v, name, x, x + name.size() + 2});
      x += name.size() + 2;
    }
    return out;
  }
  // The toggles' line: a chip per setting, its key and its state; those not at their default
  // stand out. Narrow screens drop the least used.
  struct Chip {
    Toggle toggle;
    std::string text;
    std::size_t x0 = 0, x1 = 0;
  };
  std::vector<Chip> chips() const {
    struct Want {
      Toggle toggle;
      std::string_view key;
      std::string label;
      bool set;
      int priority;
    };
    std::vector<Want> want;
    want.push_back({Toggle::filter, "/", filter_.empty() ? std::string("filter") : "filter: " + filter_, !filter_.empty(), 6});
    want.push_back({Toggle::worker, "w", tree_.worker() ? "worker " + format_worker(*tree_.worker()) : std::string("all workers"), tree_.worker().has_value(), 5});
    if (view_ == View::tree) {
      want.push_back({Toggle::order, "o", tree_.newest_first() ? "newest first" : "oldest first", false, 3});
      const std::string_view sort = tree_.order() == Tree::Order::alloc      ? "by bytes allocated"
                                    : tree_.order() == Tree::Order::retained ? "by bytes retained"
                                    : tree_.order() == Tree::Order::time     ? "by time"
                                                                             : "in start order";
      want.push_back({Toggle::sort, "s", std::string(sort), tree_.order() != Tree::Order::start, 4});
      want.push_back({Toggle::reused, "r", tree_.hide_reused() ? "reused hidden" : "reused shown", tree_.hide_reused(), 2});
      want.push_back({Toggle::source, "S", show_source_ ? "source column" : "no source column", show_source_, 1});
    }
    std::vector<Piece> pieces;
    for (const Want& w : want) pieces.push_back({paint("1;36", w.key) + " " + paint(w.set ? "1;33" : "", w.label), w.priority});
    // The pieces that fit, as `pack` keeps them, and where each lands.
    const std::size_t width = cols_ > 1 ? cols_ - 1 : 1;
    std::vector<Chip> out;
    std::vector<bool> keep(pieces.size(), true);
    const auto length = [&] {
      std::size_t n = 0;
      for (std::size_t i = 0; i < pieces.size(); ++i)
        if (keep[i]) n += columns(pieces[i].text) + 3;
      return n;
    };
    while (length() > width + 3 && std::ranges::count(keep, true) > 1) {
      std::size_t drop = pieces.size();
      for (std::size_t i = 0; i < pieces.size(); ++i)
        if (keep[i] && (drop == pieces.size() || pieces[i].priority < pieces[drop].priority)) drop = i;
      keep[drop] = false;
    }
    std::size_t x = 1;
    for (std::size_t i = 0; i < pieces.size(); ++i) {
      if (!keep[i]) continue;
      const std::size_t w = columns(pieces[i].text);
      out.push_back({want[i].toggle, pieces[i].text, x, x + w});
      x += w + 3;
    }
    return out;
  }
  // What a toggle's key does, from its chip.
  void toggle(Toggle t) {
    switch (t) {
      case Toggle::filter: common_key("/"); break;
      case Toggle::worker: common_key("w"); break;
      case Toggle::order: common_key("o"); break;
      case Toggle::sort: common_key("s"); break;
      case Toggle::reused: common_key("r"); break;
      case Toggle::source: show_source_ = !show_source_; break;
    }
  }

  // The tree pane's metric columns, right of the cell column, and their widths. Narrow panes drop
  // them from the right, so the cell keeps `min_cell` columns.
  static constexpr std::size_t min_cell = 24;
  static constexpr std::size_t source_width = 18;
  static constexpr std::array<std::pair<Column, std::size_t>, 4> metrics{
      {{Column::time, 8}, {Column::worker, 7}, {Column::alloc, 9}, {Column::retained, 9}}};
  static std::string_view column_title(Column c) {
    switch (c) {
      case Column::cell: return "cell";
      case Column::time: return "time";
      case Column::worker: return "worker";
      case Column::alloc: return "alloc";
      case Column::retained: return "retains";
      case Column::source: return "source";
    }
    return "?";
  }
  struct Span {
    Column column;
    std::size_t x0, x1;
  };
  // Each column's x range in the tree pane, the cell's first.
  std::vector<Span> column_spans() const {
    const std::size_t lw = left_width();
    std::size_t used = 0, shown = 0;
    // The source column (S, #638), when it is on.
    std::vector<std::pair<Column, std::size_t>> wanted;
    // Last, so it goes first when the pane is narrow and never hides the metrics.
    wanted.append_range(metrics);
    if (show_source_) wanted.emplace_back(Column::source, source_width);
    for (const auto& [c, w] : wanted) {
      if (lw < min_cell + used + w + 1) break;
      used += w + 1;
      ++shown;
    }
    std::vector<Span> out{{Column::cell, 0, lw - used}};
    std::size_t x = lw - used;
    for (std::size_t i = 0; i < shown; ++i) {
      out.push_back({wanted[i].first, x + 1, x + 1 + wanted[i].second});
      x += wanted[i].second + 1;
    }
    return out;
  }
  // The column `order` sorts by.
  static std::optional<Column> sorted_column(Tree::Order o) {
    switch (o) {
      case Tree::Order::start: return std::nullopt;
      case Tree::Order::alloc: return Column::alloc;
      case Tree::Order::retained: return Column::retained;
      case Tree::Order::time: return Column::time;
    }
    return std::nullopt;
  }

  // ---- drawing
  // The bars at the top and the bottom: a background of their own, kept across the resets inside.
  static constexpr std::string_view bar_sgr = "48;5;236";
  std::string on_bar(std::string_view line) const { return under(bar_sgr, fit(line, cols_)); }

  // The title bar: the name, the tabs (the current one lit), and on the right whether the trace
  // is live and followed, and what feeds it.
  std::string title_bar() const {
    std::string left = paint("1", title_name) + " ";
    for (const Tab& t : tabs()) left += view_ == t.view ? paint("1;7", std::format(" {} ", t.name)) : std::format(" {} ", t.name);
    std::vector<Piece> right;
    if (live_) {
      right.push_back({paint(running_sgr, "● live"), 3});
      right.push_back({follow_ ? paint("1", "following") : paint("2", "paused, f follows"), 2});
      // What feeds it, its path's end kept when it is long (`watching …/project`).
      const std::size_t lw = columns(left) + 2, used = 6 + 2 + (follow_ ? 9 : 17) + 2 + 1;
      if (!live_->empty() && cols_ > lw + used + 12) right.push_back({paint("2", ellipsize_path(*live_, cols_ - lw - used)), 1});
    } else if (ended_) {
      right.push_back({paint("2", *ended_), 1});
    }
    const std::size_t lw = columns(left);
    const std::string r = right.empty() || cols_ <= lw + 2 ? std::string() : pack(right, cols_ - lw - 2) + " ";
    return on_bar(spread(left, r, cols_));
  }
  // The summary, in pieces that wrap onto at most three lines (one on a short screen): each
  // outcome's count, then events and time, then memory; what does not fit is left out.
  std::vector<std::string> summary_lines() const {
    const Summary s = tree_.summary();
    std::vector<std::string> pieces;
    // While cells run (#939): how many, before the counts of those that ended.
    if (s.running > 0) pieces.push_back(paint(running_sgr, std::format("▶ {} running", s.running)));
    for (std::uint8_t i = 0; i < outcome_count; ++i) {
      const auto o = static_cast<Outcome>(i);
      if (s.counts[i] == 0) continue;
      pieces.push_back(paint(outcome_sgr(o), std::format("{} {} {}", outcome_glyph(o, true), s.counts[i], outcome_name(o))));
    }
    pieces.push_back(paint("2", std::format("{} events, {} workers, {}", s.events, s.workers, format_duration(s.span_ns))));
    // Memory (#274): what the frames allocated and the cells retain; the process's, when known.
    pieces.push_back(paint("2", std::format("{} allocated, {} retained", format_bytes(static_cast<std::int64_t>(s.alloc_bytes)), format_bytes(s.retained_bytes))));
    if (const auto m = tree_.memory())
      pieces.push_back(paint("2", std::format("{}: process {}, peak {}", s.running > 0 ? "this build so far" : "last build", format_bytes(m->process_current),
                                              format_bytes(m->process_peak))));
    const std::size_t most = rows_ < 16 ? 1 : 3;
    std::vector<std::string> out;
    for (const std::string& p : pieces) {
      // A piece goes on the first line it fits on, so a short one after a long one still shows.
      bool placed = false;
      for (std::string& l : out) {
        if (columns(l) + 2 + columns(p) <= cols_) {
          l += "  " + p;
          placed = true;
          break;
        }
      }
      if (placed) continue;
      if (out.size() < most) out.push_back(" " + ellipsize(p, cols_ > 1 ? cols_ - 1 : 1));
    }
    if (out.empty()) out.emplace_back();
    return out;
  }
  // Everything above the body: the title bar, the summary, the toggles.
  std::vector<std::string> header_lines() const {
    std::vector<std::string> out{title_bar()};
    out.append_range(summary_lines());
    out.push_back(toggles_line());
    return out;
  }
  std::string toggles_line() const {
    std::string out;
    std::size_t x = 0;
    for (const Chip& c : chips()) {
      out += std::string(c.x0 - x, ' ') + c.text;
      x = c.x1;
    }
    return out;
  }
  // The footer: a note when there is one, then the keys that matter in the view (as many as fit),
  // and help and quit always, on the right.
  struct Hint {
    std::string_view keys, what;
    int priority;
  };
  std::vector<Hint> hints() const {
    if (visual_) return {{"j/k", "extend", 9}, {"y", "copy", 9}, {"Esc", "drop", 9}};
    if (view_ == View::tree && pane_focused())
      return {{"↑↓", "scroll", 9}, {"PgUp/PgDn", "page", 7}, {"g/G", "top/end", 6}, {"V", "select lines", 5}, {"y", "copy", 8}, {"←/Esc", "back to the tree", 8}};
    if (view_ == View::tree)
      return {{"↑↓", "move", 9},       {"⏎", "fold", 8},       {"←→", "close/open", 4}, {"/", "filter", 8},      {"n", "next run", 6},
              {"x", "next error", 6},  {"e/E", "open/close all", 3}, {"y/Y", "copy", 5},     {"J/K", "scroll pane", 2}, {"v", "pane", 2},
              {"t", "timeline", 7},    {"b", "histogram", 7}};
    if (view_ == View::histogram)
      return {{"↑↓", "group", 9}, {"⏎", "its cells", 8}, {"1-9", "metrics", 7}, {"c", "group by", 7}, {"s", "sort", 6}, {"i", "in the cell", 4},
              {"/", "filter", 5}, {"b", "tree", 8}};
    return {{"↑↓", "lane", 9}, {"←→", "scrub", 9}, {"H/L", "by 10", 4}, {"⏎", "open in tree", 8}, {"/", "filter", 5}, {"t", "tree", 8}, {"b", "histogram", 6}};
  }
  static std::string hint_text(std::string_view keys, std::string_view what) { return paint("1", keys) + " " + std::string(what); }
  std::string footer() const {
    if (prompt_) {
      // What is typed comes first: the hint shows only beside it, and a long filter shows its end.
      const std::string shown = prompt_shown();
      std::string hint = paint("2", "⏎ keep  Esc cancel  ? help ");
      if (prompt_label.size() + columns(shown) + 2 + columns(hint) > cols_) hint.clear();
      return on_bar(spread(std::string(prompt_label) + shown, hint, cols_));
    }
    std::vector<Piece> left;
    if (!notice_.empty()) left.push_back({paint("1;32", " " + notice_), 100});
    if (visual_) left.push_back({paint("1;7", " VISUAL "), 99});
    for (const Hint& h : hints()) left.push_back({hint_text(h.keys, h.what), h.priority});
    const std::string right = hint_text("?", "help") + "  " + hint_text("q", "quit") + " ";
    const std::size_t room = cols_ > columns(right) + 2 ? cols_ - columns(right) - 2 : 1;
    return on_bar(spread(" " + pack(left, room), right, cols_));
  }
  static constexpr std::string_view prompt_label = " / ";
  // The filter as typed, its end kept when it is longer than the footer (a column stays for the
  // cursor).
  std::string prompt_shown() const {
    const std::size_t room = cols_ > prompt_label.size() + 1 ? cols_ - prompt_label.size() - 1 : 1;
    const std::size_t n = columns(typed_);
    if (n <= room) return typed_;
    std::size_t at = typed_.size(), col = 0;
    while (at > 0 && col + 1 < room) {
      --at;
      if ((static_cast<unsigned char>(typed_[at]) & 0xc0) != 0x80) ++col;
    }
    return "…" + typed_.substr(at);
  }

  std::string metric(const Tree::Node& n, Column c) const {
    const Event& e = n.ev;
    switch (c) {
      case Column::time: return e.end_ns == e.start_ns ? std::string() : format_duration(e.end_ns - e.start_ns);
      case Column::worker: return n.group ? std::string() : format_workers(e);
      case Column::alloc: return n.incl.alloc > 0 ? format_bytes(static_cast<std::int64_t>(n.incl.alloc)) : std::string();
      case Column::retained: return n.incl.retained > 0 ? format_bytes(n.incl.retained) : std::string();
      case Column::source: {
        // The file's last component and the line: the whole path is in the pane, and `p` copies it.
        if (e.span.file.empty()) return {};
        const std::string_view f = e.span.file;
        const std::string_view base = f.substr(f.rfind('/') == std::string_view::npos ? 0 : f.rfind('/') + 1);
        return e.span.line == 0 ? std::string(base) : std::format("{}:{}", base, e.span.line);
      }
      case Column::cell: return {};
    }
    return {};
  }
  // The cell column, in `width` columns: indentation, the marker, the outcome's glyph, the cell,
  // its outcome, and what is folded under it. When it does not fit, the count goes short
  // (`[+20, 1 ran]`, `[+20]`), then the outcome's name (its glyph and color stay), then the key
  // is cut with `…`: never mid-word into the next column.
  std::string cell_text(const Tree::Row& r, std::size_t width) const {
    const Tree::Node& n = tree_.node(r.node);
    const Event& e = n.ev;
    std::string lead(2 * r.depth, ' ');
    lead += n.children.empty() ? "  " : (n.expanded ? "▾ " : "▸ ");
    if (tree_.error_here(r.node)) {
      // Where the errors start: the whole cell on red, and the first message on it.
      return ellipsize(lead + paint(error_here_sgr, std::format("{} {} {} ← {}", state_glyph(e, true), cell_name(e), state_name(e), first_line(e.errors))),
                       width);
    }
    const std::string name = paint(event_sgr(e), state_glyph(e, true)) + ' ' +
                             std::format("{}{}", paint(r.match ? "1" : "2", e.kind), e.key.empty() ? "" : std::format("({})", e.key));
    const std::string state = ' ' + paint(event_sgr(e), state_name(e));
    std::string extra;
    if (e.repeat > 1) extra += paint("2", std::format(" ×{}", e.repeat));
    if (!e.errors.empty()) {
      // The message from where it starts, bubbled up; `x` goes there.
      const auto at = tree_.error_origin_below(r.node);
      extra += paint(error_sgr, std::format("  (error below: {})", first_line(at ? tree_.node(*at).ev.errors : e.errors)));
    }
    std::vector<std::string> folds{""};
    if (!n.expanded && n.children.size() > 0) {
      const std::uint64_t below = n.subtree - 1, ran = ran_below(n);
      folds = {paint("2", std::format("  [{} below, {} ran]", below, ran)), paint("2", std::format("  [+{}, {} ran]", below, ran)),
               paint("2", std::format("  [+{}]", below)), ""};
    }
    const std::size_t fixed = columns(lead) + columns(name);
    for (const std::string& f : folds)
      if (fixed + columns(state) + columns(extra) + columns(f) <= width) return lead + name + state + extra + f;
    if (fixed + columns(state) <= width) return ellipsize(lead + name + state + extra, width);
    return ellipsize(lead + name, width);
  }
  std::string tree_row(const Tree::Row& r, const std::vector<Span>& spans) const {
    const Tree::Node& n = tree_.node(r.node);
    // A column of space before the metrics, so a cut cell never touches them.
    const std::size_t w = spans.size() > 1 && spans[0].x1 > 1 ? spans[0].x1 - 1 : spans[0].x1;
    std::string out = fit(cell_text(r, w), spans[0].x1);
    for (std::size_t i = 1; i < spans.size(); ++i) out += ' ' + paint("2", pad_left(metric(n, spans[i].column), spans[i].x1 - spans[i].x0));
    return out;
  }
  std::string column_headers(const std::vector<Span>& spans) const {
    const std::optional<Column> sorted = sorted_column(tree_.order());
    const auto title = [&](Column c) {
      return std::string(column_title(c)) + (sorted == c || (!sorted && c == Column::cell) ? "↓" : "");
    };
    const std::string_view sgr = !pane_focused() && split() ? "1;4" : "1";
    std::string out = paint(sgr, fit(" " + title(Column::cell), spans[0].x1));
    for (std::size_t i = 1; i < spans.size(); ++i) out += ' ' + paint(sorted == spans[i].column ? "1;4" : "1", pad_left(title(spans[i].column), spans[i].x1 - spans[i].x0));
    return out;
  }

  // Executions under a node, not counting its own (a request's node is no execution).
  static std::uint64_t ran_below(const Tree::Node& n) {
    const bool own = !n.group && (n.ev.outcome == Outcome::executed || n.ev.outcome == Outcome::cutoff);
    return n.executions - (own ? 1 : 0);
  }

  std::vector<std::string> tree_body() {
    const auto rows = tree_.rows();
    const std::size_t hgt = list_height();
    if (!rows.empty()) cursor_ = std::min(cursor_, rows.size() - 1);
    else cursor_ = 0;
    top_ = std::min(top_, rows.size() > hgt ? rows.size() - hgt : 0);
    if (cursor_ < top_) top_ = cursor_;
    if (cursor_ >= top_ + hgt) top_ = cursor_ - hgt + 1;
    const std::vector<Span> spans = column_spans();
    const std::size_t lw = left_width();
    std::vector<std::string> left{column_headers(spans)};
    std::size_t sel_lo = 1, sel_hi = 0;  // none
    if (rows_sel_)
      if (const auto r = row_range(*rows_sel_)) std::tie(sel_lo, sel_hi) = *r;
    for (std::size_t i = top_; i < rows.size() && left.size() <= hgt; ++i) {
      std::string line = tree_row(rows[i], spans);
      if (i == cursor_ || (i >= sel_lo && i <= sel_hi)) line = selected(fit(line, lw));
      left.push_back(std::move(line));
    }
    if (rows.empty()) left.push_back(paint("2", "  nothing matches"));
    if (!split()) return left;
    const std::vector<PaneLine> right = rows.empty() ? std::vector<PaneLine>{} : detail(tree_.node(rows[cursor_].node), pane_width());
    const std::size_t rw = cols_ - lw - 1;
    // A pane longer than the screen keeps its last line for where it is and how to move.
    const bool long_pane = right.size() > hgt && hgt > 1;
    const std::size_t shown = long_pane ? hgt - 1 : hgt;
    detail_scroll_ = right.size() > shown ? std::min(detail_scroll_, right.size() - shown) : 0;
    std::vector<std::string> out;
    const std::string_view title_sgr = pane_focused() ? "1;4" : "1";
    out.push_back(fit(left[0], lw) + paint(dragging() ? "1;7" : "2", "│") + paint(title_sgr, fit(" cell", rw)));
    for (std::size_t i = 0; i < hgt; ++i) {
      std::string l = i + 1 < left.size() ? left[i + 1] : std::string();
      const std::size_t at = i + detail_scroll_;
      std::string r = at < right.size() ? right[at].text : std::string();
      if (text_sel_ && at < right.size()) {
        const auto [from, to] = covered(*text_sel_, at, columns(r));
        if (from < to) r = highlight(r, from, to);
      }
      if (long_pane && i == hgt - 1)
        r = paint("2;7", fit(std::format(" {}–{} of {}  J/K ]/[ }}/{{ scroll", detail_scroll_ + 1, detail_scroll_ + shown, right.size()), rw));
      out.push_back(fit(l, lw) + paint(dragging() ? "1;7" : "2", "│") + fit(r, rw));
    }
    return out;
  }

  // The pane's lines for a node, wrapped at `rw` columns, each after a margin of one.
  std::vector<PaneLine> detail(const Tree::Node& n, std::size_t rw) const {
    const Event& e = n.ev;
    std::vector<PaneLine> out;
    // Every line wraps at the pane's width, so a selection never covers text past its edge.
    const auto wrapped = [&](std::string_view text, std::string_view sgr) {
      if (text.empty()) {
        out.push_back({});
        return;
      }
      for (auto part : text | std::views::split('\n')) {
        bool first = true;
        for (const std::string& l : wrap(std::string_view(part.begin(), part.end()), rw)) {
          out.push_back({" " + (sgr.empty() ? l : paint(sgr, l)), !first});
          first = false;
        }
      }
    };
    const auto add = [&](std::string_view text) { wrapped(text, ""); };
    wrapped(e.kind, "1");
    wrapped(e.key.empty() ? "(no key)" : e.key, "");
    add("");
    wrapped(std::format("{} {}", state_glyph(e, true), state_name(e)), event_sgr(e));
    wrapped(state_meaning(e), "2");
    add("");
    add(std::format("started  {}", format_duration(e.start_ns)));
    add(std::format("took     {}", e.end_ns == e.start_ns ? std::string("(instant)") : format_duration(e.end_ns - e.start_ns)));
    add(std::format("worker   {}", format_workers(e)));
    add(std::format("depth    {}", e.depth));
    add(std::format("below    {} cells, {} ran", n.subtree - 1, ran_below(n)));
    if (n.parent >= 0) add("for      " + cell_name(tree_.node(n.parent).ev));
    if (!e.span.file.empty()) add("source   " + span_text(e.span));  // #638
    add("");
    // Memory (#274): this frame's own, its subtree's, and what the cell retains.
    if (e.alloc_bytes > 0 || n.incl.alloc > 0 || e.retained_bytes != 0) {
      // A table: the cell's own frame, and with everything below it. A request's own is nothing:
      // its cells did the work, and the second column says how much.
      wrapped("memory", "1;4");
      const std::array<std::array<std::string, 3>, 4> rows{{
          {"alloc", format_bytes(static_cast<std::int64_t>(e.alloc_bytes)), format_bytes(static_cast<std::int64_t>(n.incl.alloc))},
          {"allocs", std::format("{}", e.allocs), ""},
          {"peak", format_bytes(e.peak_bytes), ""},
          {"retains", format_bytes(e.retained_bytes), format_bytes(n.incl.retained)},
      }};
      std::size_t own = 3, all = 6;
      for (const auto& r : rows) own = std::max(own, r[1].size()), all = std::max(all, r[2].size());
      const auto line = [&](std::string_view a, std::string_view b, std::string_view c) {
        std::string l = std::format("{:<9}{:>{}}  {:>{}}", a, b, own, c, all);
        while (l.ends_with(' ')) l.pop_back();
        return l;
      };
      wrapped(line("", "own", "in all"), "2");
      for (const auto& r : rows) add(line(r[0], r[1], r[2]));
      add("");
    }
    if (!e.errors.empty()) {
      wrapped("errors", "1;4;31");
      wrapped(e.errors, error_sgr);
      add("");
    }
    if (!e.value.empty()) {
      wrapped("value", "1;4");
      wrapped(e.value, "");
    } else {
      wrapped("no value shown for this kind", "2");
    }
    return out;
  }

  // ---- the histogram (#637)
  // By children, the selected cell's; else, with `i`, the selected cell's subtree.
  std::optional<std::int32_t> bars_within() const {
    return within_ || group_by_ == Grouping::children ? selected_node() : std::nullopt;
  }
  std::vector<Group> bars() const { return histogram(tree_, group_by_, bars_within(), sort_by_); }
  std::vector<Metric> shown_metrics() const {
    std::vector<Metric> out;
    for (std::size_t i = 0; i < metric_count; ++i)
      if (metrics_on_[i]) out.push_back(static_cast<Metric>(i));
    return out;
  }
  void toggle_metric(Metric m) {
    const std::size_t i = std::to_underlying(m);
    if (metrics_on_[i] && shown_metrics().size() == 1) return;  // one stays
    metrics_on_[i] = !metrics_on_[i];
    if (!metrics_on_[std::to_underlying(sort_by_)]) sort_by_ = shown_metrics().front();
  }
  // The tree shows the bar's cells: a kind or a file added to the filter (as the bar counted them,
  // under the same filter), a child as the selection; with `i`, the selected cell stays selected.
  void open_bar() {
    const std::vector<Group> gs = bars();
    if (bar_ >= gs.size()) return;
    const Group& g = gs[bar_];
    const std::optional<std::int32_t> within = bars_within();
    if (group_by_ == Grouping::children) {
      if (g.node < 0) return;
      view_ = View::tree;
      select(g.node);
      return;
    }
    if (group_by_ == Grouping::file && g.name == "(no source)") return;
    // The user's words stay; a word of the same kind as the bar's (an earlier bar's) gives way.
    const std::string_view prefix = group_by_ == Grouping::kind ? "kind:" : "at:";
    std::vector<std::string> words;
    for (std::string& w : filter_words(filter_))
      if (!w.starts_with(prefix)) words.push_back(std::move(w));
    words.push_back(group_by_ == Grouping::kind ? "kind:" + g.name : "at:=" + g.name);
    filter_ = typed_ = filter_text(words);
    tree_.set_filter(filter_);
    clear_selection();
    cursor_ = 0;
    view_ = View::tree;
    if (within) select(*within);
  }
  bool histogram_key(const std::string& k) {
    const std::size_t n = bars().size();
    if (k == "j" || k == "\x1b[B" || k == "\x1bOB") bar_ = n ? std::min(bar_ + 1, n - 1) : 0;
    else if (k == "k" || k == "\x1b[A" || k == "\x1bOA") bar_ = bar_ > 0 ? bar_ - 1 : 0;
    else if (k == "g" || k == "\x1b[H") bar_ = 0;
    else if (k == "G" || k == "\x1b[F") bar_ = n ? n - 1 : 0;
    else if (k.size() == 1 && k[0] >= '1' && k[0] <= '9') toggle_metric(static_cast<Metric>(k[0] - '1'));
    else if (k == "0") {}  // digits are the metrics here, not workers
    else if (k == "c") group_by_ = static_cast<Grouping>((std::to_underlying(group_by_) + 1) % grouping_count), bar_ = bars_top_ = 0;
    else if (k == "i") within_ = !within_, bar_ = bars_top_ = 0;
    else if (k == "s") {
      const std::vector<Metric> on = shown_metrics();
      const auto it = std::ranges::find(on, sort_by_);
      sort_by_ = it == on.end() || it + 1 == on.end() ? on.front() : *(it + 1);
    } else if (k == "\r" || k == " ") open_bar();
    else return false;
    return true;
  }
  static constexpr std::size_t bar_name_width = 28;
  static std::string_view grouping_name(Grouping g) {
    switch (g) {
      case Grouping::kind: return "kind";
      case Grouping::file: return "file";
      case Grouping::children: return "children";
    }
    return "?";
  }
  // A metric's switch: short, so all nine fit.
  static std::string_view switch_name(Metric m) {
    switch (m) {
      case Metric::self_time: return "self";
      case Metric::total_time: return "in all";
      case Metric::alloc: return "alloc";
      case Metric::peak: return "peak";
      case Metric::retained: return "retained";
      case Metric::allocs: return "allocs";
      case Metric::executions: return "runs";
      case Metric::hits: return "hits";
      case Metric::waits: return "waits";
    }
    return "?";
  }
  struct Item {
    std::size_t row, x0, x1;  // row in the body
    Hit hit;
    std::string text;
  };
  // The switches: each metric, numbered as its key, on as many lines as the width needs.
  std::vector<Item> metric_items() const {
    std::vector<Item> out;
    std::size_t row = 0, x = 10;  // after ` metrics: `
    for (std::size_t i = 0; i < metric_count; ++i) {
      std::string text = std::format("{}{} {}", metrics_on_[i] ? "■" : "□", i + 1, switch_name(static_cast<Metric>(i)));
      const std::size_t w = columns(text);
      if (x + w > cols_ && x > 10) ++row, x = 10;
      out.push_back({row, x, x + w, {Hit::Area::metric, i}, std::move(text)});
      x += w + 2;
    }
    return out;
  }
  std::size_t switch_rows() const { return metric_items().back().row + 1; }
  // The grouping's line: by kind, file or children; and the scope (by children: the cell).
  std::vector<Item> grouping_items() const {
    std::vector<Item> out;
    const std::size_t row = switch_rows();
    std::size_t x = 11;  // after ` group by: `
    for (std::size_t i = 0; i < grouping_count; ++i) {
      std::string text(grouping_name(static_cast<Grouping>(i)));
      out.push_back({row, x, x + text.size(), {Hit::Area::grouping, i}, text});
      x += text.size() + 2;
    }
    x += 3;
    std::string scope = group_by_ == Grouping::children ? "of the selected cell" : within_ ? "in the selected cell" : "in everything";
    out.push_back({row, x, x + scope.size(), {group_by_ == Grouping::children ? Hit::Area::none : Hit::Area::scope}, scope});
    return out;
  }
  // The columns after the group's name, one per metric shown that fits, each a bar and its value
  // in a width every row shares (so bars of one column share one scale).
  struct BarColumn {
    Metric metric;
    std::size_t x0, x1, bar_width;
  };
  std::vector<BarColumn> bar_columns(const std::vector<Group>& gs) const {
    const std::vector<Metric> on = shown_metrics();
    // A column of margin on the right, so the last value never touches the edge.
    const std::size_t avail = cols_ > bar_name_width + 2 ? cols_ - bar_name_width - 2 : 0;
    const std::size_t w = on.empty() ? 0 : std::max<std::size_t>(14, avail / on.size());
    std::vector<BarColumn> out;
    std::size_t x = bar_name_width + 1;
    for (Metric m : on) {
      if (x + w + 1 > cols_) break;
      std::size_t value_width = 3;
      for (const Group& g : gs) value_width = std::max(value_width, columns(format_metric(m, g.at(m))));
      const std::size_t inner = w - 1;
      out.push_back({m, x, x + inner, inner > value_width + 1 ? inner - value_width - 1 : 0});
      x += w;
    }
    return out;
  }
  // Rows of the body: the switches, the grouping, the headers, then the bars.
  std::size_t bar_rows_from() const { return switch_rows() + 2; }
  std::size_t bar_rows() const { return body_height() > bar_rows_from() ? body_height() - bar_rows_from() : 1; }
  Hit histogram_hit(std::size_t x, std::size_t by) const {
    const auto in = [&](const std::vector<Item>& items) -> Hit {
      for (const Item& it : items)
        if (it.row == by && x >= it.x0 && x < it.x1) return it.hit;
      return {};
    };
    const std::size_t sr = switch_rows();
    if (by < sr) return in(metric_items());
    if (by == sr) return in(grouping_items());
    if (by == sr + 1) {
      for (const BarColumn& c : bar_columns(bars()))
        if (x >= c.x0 && x < c.x1) return {Hit::Area::order, std::to_underlying(c.metric)};
      return {};
    }
    const std::size_t i = bars_top_ + (by - bar_rows_from());
    return i < bars().size() ? Hit{Hit::Area::bar, i} : Hit{};
  }
  // A bar of `width` columns, `v` of `most` of it, in eighths.
  static std::string bar(std::uint64_t v, std::uint64_t most, std::size_t width) {
    static constexpr std::array<std::string_view, 8> part{"", "▏", "▎", "▍", "▌", "▋", "▊", "▉"};
    const std::uint64_t eighths = most == 0 ? 0 : (v * width * 8 + most - 1) / most;
    std::string out;
    for (std::uint64_t i = 0; i < eighths / 8; ++i) out += "█";
    out += part[eighths % 8];
    return out;
  }
  std::vector<std::string> histogram_body() {
    const std::vector<Group> gs = bars();
    bar_ = gs.empty() ? 0 : std::min(bar_, gs.size() - 1);
    const std::size_t rows = bar_rows();
    bars_top_ = std::min(bars_top_, gs.size() > rows ? gs.size() - rows : 0);
    if (bar_ < bars_top_) bars_top_ = bar_;
    if (bar_ >= bars_top_ + rows) bars_top_ = bar_ - rows + 1;
    std::vector<std::string> out;
    for (const Item& it : metric_items()) {
      if (out.size() <= it.row) out.push_back(it.row == 0 ? " metrics: " : std::string(10, ' '));
      out[it.row] += paint(metrics_on_[it.hit.index] ? "1" : "2", it.text) + "  ";
    }
    std::string line = " group by: ";
    for (const Item& it : grouping_items()) {
      if (it.hit.area == Hit::Area::grouping) line += paint(static_cast<Grouping>(it.hit.index) == group_by_ ? "1;4" : "2", it.text) + "  ";
      else line += "   " + paint("1;4", it.text);
    }
    if (const auto w = bars_within()) line += paint("2", "  " + cell_name(tree_.node(*w).ev));
    const auto cols = bar_columns(gs);
    line += paint("2", std::format("   sorted by {}", metric_name(sort_by_)));
    if (cols.size() < shown_metrics().size()) line += paint("2", std::format(", {} hidden: too narrow", shown_metrics().size() - cols.size()));
    out.push_back(line);
    line = paint("1", fit(std::format(" {} ({})", grouping_name(group_by_), gs.size()), bar_name_width)) + " ";
    for (const BarColumn& c : cols) {
      const std::string title = std::string(metric_name(c.metric)) + (c.metric == sort_by_ ? "↓" : "");
      line += paint(c.metric == sort_by_ ? "1;4" : "1", fit(title, c.x1 - c.x0)) + " ";
    }
    out.push_back(line);
    std::array<std::uint64_t, metric_count> most{};
    for (const Group& g : gs)
      for (std::size_t i = 0; i < metric_count; ++i) most[i] = std::max(most[i], g.v[i]);
    for (std::size_t i = bars_top_; i < gs.size() && i < bars_top_ + rows; ++i) {
      const Group& g = gs[i];
      line = fit(ellipsize(std::format(" {} ×{}", g.name, g.cells), bar_name_width - 1), bar_name_width) + " ";
      for (const BarColumn& c : cols) {
        const std::size_t w = c.x1 - c.x0;
        line += paint("36", fit(bar(g.at(c.metric), most[std::to_underlying(c.metric)], c.bar_width), c.bar_width)) + " " +
                pad_left(format_metric(c.metric, g.at(c.metric)), w - c.bar_width - 1) + " ";
      }
      out.push_back(i == bar_ ? selected(fit(line, cols_)) : line);
    }
    if (gs.empty()) out.push_back(paint("2", "  nothing matches"));
    return out;
  }

  std::vector<std::string> timeline_body() {
    const std::size_t cols = lane_columns();
    time_ = std::min(time_, cols - 1);
    const Timeline tl = timeline(tree_, cols);
    lane_ = std::min(lane_, tl.lanes.size() - 1);
    std::vector<std::string> out;
    const auto [from, to] = tl.slice(time_, cols);
    out.push_back(paint("2", std::string(lane_prefix, ' ') + axis(tl.span_ns, cols)));
    for (std::size_t lane = 0; lane < tl.lanes.size(); ++lane) {
      std::string line = std::format("{}{:>6} │", lane == lane_ ? "▶" : " ", format_worker(Timeline::worker_of(lane)));
      for (std::size_t c = 0; c < cols; ++c) {
        const std::int32_t node = tl.lanes[lane][c];
        const bool cur = c == time_;
        if (node < 0) {
          line += cur ? paint("7;2", "│") : paint("2", "·");
          continue;
        }
        const Event& e = tree_.node(node).ev;
        // Deeper cells darker: the shade says how far down the demand chain the worker was.
        const std::string_view block = e.depth == 0 ? "█" : e.depth == 1 ? "▓" : e.depth == 2 ? "▒" : "░";
        line += paint(std::format("{}{}", event_sgr(e), cur ? ";7" : ""), block);
      }
      out.push_back(std::move(line));
    }
    out.emplace_back();
    out.push_back(paint("1", std::format(" at {} – {}", format_duration(from), format_duration(to))));
    // What each worker was in then; workers next to each other in the same cell share a line.
    struct Line {
      std::size_t first, last;
      std::string text;
    };
    std::vector<Line> lines;
    for (std::size_t lane = 0; lane < tl.lanes.size(); ++lane) {
      const std::string text = slice_text(tl.lanes[lane][time_], cols_ > 12 ? cols_ - 12 : 1);
      if (!lines.empty() && lines.back().text == text && lines.back().last + 1 == lane) lines.back().last = lane;
      else lines.push_back({lane, lane, text});
    }
    for (const Line& l : lines) {
      const std::string who = l.first == l.last ? format_worker(Timeline::worker_of(l.first))
                                                : std::format("{}–{}", format_worker(Timeline::worker_of(l.first)), format_worker(Timeline::worker_of(l.last)));
      const bool here = lane_ >= l.first && lane_ <= l.last;
      out.push_back(std::format(" {}{:>8}  {}", here ? "▶" : " ", who, l.text));
    }
    out.emplace_back();
    std::vector<std::string> legend;
    for (std::uint8_t i = 0; i < outcome_count; ++i) {
      const auto o = static_cast<Outcome>(i);
      legend.push_back(paint(outcome_sgr(o), "█") + " " + std::string(outcome_name(o)));
    }
    legend.push_back(paint(running_sgr, "█") + " running");
    legend.push_back(paint("2", "█▓▒░ shallower to deeper"));
    std::string line = " ";
    for (const std::string& l : legend) {
      if (columns(line) + columns(l) + 2 > cols_ && line != " ") out.push_back(line), line = " ";
      line += l + "  ";
    }
    out.push_back(line);
    return out;
  }
  // The axis under the lanes: times at a tick every 20 columns or so, the end's at the end.
  static std::string axis(std::uint64_t span_ns, std::size_t cols) {
    std::string out(cols, ' ');
    const auto at = [&](std::size_t c) { return cols > 1 ? span_ns * c / (cols - 1) : 0; };
    const std::string end = format_duration(span_ns);
    const std::size_t end_x = cols > columns(end) ? cols - columns(end) : 0;
    std::vector<std::pair<std::size_t, std::string>> labels;
    for (std::size_t c = 0; c + 1 < cols; c += 20) {
      std::string t = c == 0 ? std::string("0") : format_duration(at(c));
      if (c + columns(t) + 2 > end_x) break;
      labels.emplace_back(c, "╵" + t);
    }
    labels.emplace_back(end_x > 0 ? end_x - 1 : 0, end + "╵");
    std::string line;
    std::size_t x = 0;
    for (const auto& [c, t] : labels) {
      if (c < x) continue;
      line += std::string(c - x, ' ') + t;
      x = c + columns(t);
    }
    return fit(line, cols);
  }
  // A slice of a lane: the cell, its outcome, and the chain of demand down to it, cut in the
  // middle to fit `width`; or idle.
  std::string slice_text(std::int32_t node, std::size_t width) const {
    if (node < 0) return paint("2", "idle");
    const Event& e = tree_.node(node).ev;
    const std::string cell = paint(event_sgr(e), state_glyph(e, true)) + " " + cell_name(e) + " " + paint(event_sgr(e), state_name(e));
    // The chain from the top-level request down to the cell's parent, so a slice says why.
    std::vector<std::string> chain;
    for (std::int32_t p = tree_.node(node).parent; p >= 0; p = tree_.node(p).parent) chain.push_back(tree_.node(p).ev.kind);
    std::ranges::reverse(chain);
    const std::size_t room = width > columns(cell) + 6 ? width - columns(cell) - 6 : 0;
    const auto joined = [&] { return chain | std::views::join_with(std::string_view(" › ")) | std::ranges::to<std::string>(); };
    bool elided = false;
    while (chain.size() > 2 && columns(joined()) > room) {
      // Out of the middle: the request it serves and the frames nearest the cell say the most.
      chain.erase(chain.begin() + (elided ? 2 : 1));
      if (!elided) chain.insert(chain.begin() + 1, "…"), elided = true;
      if (chain.size() <= 3 && elided) break;
    }
    if (room < 4) return cell;
    return cell + paint("2", "   in " + ellipsize(joined(), room));
  }

  // The help: every key and symbol, in a box over the screen, a section per subject. Taller than
  // the screen, it scrolls (↑↓, PgUp/PgDn); any other key or a click closes it.
  std::vector<std::string> help_lines(std::size_t width) const {
    using Row = std::pair<std::string, std::string_view>;
    std::vector<std::pair<std::string_view, std::vector<Row>>> sections;
    std::vector<Row> outcomes;
    for (std::uint8_t i = 0; i < outcome_count; ++i) {
      const auto o = static_cast<Outcome>(i);
      outcomes.push_back({paint(outcome_sgr(o), std::format("{} {}", outcome_glyph(o, true), outcome_name(o))), outcome_meaning(o)});
    }
    outcomes.push_back({paint(running_sgr, "▶ running"), "started, not ended yet (while the build runs)"});
    sections.push_back({"What the engine did with each cell", std::move(outcomes)});
    sections.push_back({"Views", {{"t  Tab", "the tree and the timeline"}, {"b", "the histogram, and back"}, {"click", "a tab"}}});
    sections.push_back({"Tree",
                        {{"↑↓  j/k", "move; PgUp/PgDn a page, g/G the top and the end"},
                         {"⏎  space", "fold or unfold; ←→ (h/l) close and open, ← again goes to the parent"},
                         {"e  E", "open everything, close everything"},
                         {"n  x", "the next cell that ran, the next error"},
                         {"s", "sort siblings: as they started, bytes allocated, bytes retained, time (or click a header)"},
                         {"o", "newest or oldest request first"},
                         {"r", "hide the reused cells (what ran stays)"},
                         {"S", "a column of where each cell is in the source"},
                         {"columns", "alloc and retains sum over each cell's subtree"}}});
    sections.push_back({"The pane on the right",
                        {{"J/K", "a line (j/k move in the tree); ]/[ a page, }/{ the end and the top"},
                         {"<  >", "move the divider (or drag it); v hides and shows the pane"},
                         {"click", "focuses it: then ↑↓ scroll it, ← or Esc give the keys back"}}});
    sections.push_back({"Filter (/)",
                        {{"words", "match a cell's kind or key, all of them"},
                         {"is:exec", "an outcome (any prefix); is:running the cells still running"},
                         {"at:a.cpp:12", "cells whose source touches line 12 of a.cpp (at:a.cpp: any line)"},
                         {"kind:Lower", "one kind, whole (what a histogram's bar opens)"},
                         {"w:3  w:main", "a worker"}}});
    sections.push_back({"Workers", {{"w", "cycle through them"}, {"0-9  m  a", "pick one, the requesting thread, all"}}});
    sections.push_back({"Copy (OSC 52, and the system clipboard when local)",
                        {{"y  Y", "the cell as the pane shows it; its subtree as the stream prints it"},
                         {"p", "where the cell is: path:line:col"},
                         {"V", "select rows (or the pane's lines), j/k extend, y copies"},
                         {"drag", "over rows or the pane's text: selects and copies"}}});
    sections.push_back({"Timeline",
                        {{"↑↓  ←→", "a lane, and scrub through time; H/L by ten"}, {"⏎", "the cell under the cursor, in the tree"}, {"click", "a slice; a double click opens it"}}});
    sections.push_back({"Histogram",
                        {{"1-9", "switch metrics"},
                         {"c", "group by kind, file, or the selected cell's children"},
                         {"i", "only within the selected cell"},
                         {"s", "sort by the next metric shown (or click a header)"},
                         {"⏎", "the group's cells in the tree (a kind: or at: filter)"}}});
    if (live_) sections.push_back({"While the build runs", {{"f", "follow the newest activity; any move of yours stops following"}}});
    std::size_t key_w = 0;
    for (const auto& [_, rows] : sections)
      for (const auto& [k, _] : rows) key_w = std::max(key_w, columns(k));
    key_w = std::min<std::size_t>(key_w, width / 3);
    const std::size_t what_w = width > key_w + 3 ? width - key_w - 3 : 1;
    std::vector<std::string> out;
    for (const auto& [title, rows] : sections) {
      if (!out.empty()) out.emplace_back();
      out.push_back(paint("1;4", title));
      for (const auto& [k, what] : rows) {
        bool first = true;
        for (const std::string& l : wrap(what, what_w)) {
          out.push_back(" " + (first ? paint("1", fit(k, key_w)) : std::string(key_w, ' ')) + "  " + l);
          first = false;
        }
      }
    }
    return out;
  }
  // The box: between the title bar and the footer, at most 100 wide.
  std::size_t help_box_height() const { return rows_ > 4 ? rows_ - 2 : rows_; }
  // Lines of help the box shows: its height less its borders.
  std::size_t help_room() const { return help_box_height() > 2 ? help_box_height() - 2 : 1; }
  static constexpr std::string_view help_sgr = "48;5;235";
  void overlay_help(std::vector<std::string>& lines) const {
    const std::size_t w = std::min<std::size_t>(cols_, 100);
    const std::size_t x = (cols_ - w) / 2, inner = w > 4 ? w - 4 : 1;
    const std::vector<std::string> help = help_lines(inner);
    const std::size_t room = help_room();
    const std::size_t h = std::min(help.size(), room);
    help_scroll_ = std::min(help_scroll_, help.size() - h);
    const std::size_t top = 2 + (room - h) / 2;
    const auto row = [&](std::size_t y, std::string text) {
      if (y >= lines.size()) return;
      // Left of the box the screen stays; right of it, too.
      std::string plain = fit(lines[y], cols_);
      lines[y] = fit(plain, x) + under(help_sgr, text) + "\x1b[0m" + slice_after(plain, x + w);
    };
    const std::string title = help.size() > h ? std::format(" keys {}–{} of {}: ↑↓ scroll, any other key closes ", help_scroll_ + 1, help_scroll_ + h, help.size())
                                              : std::string(" keys: any key closes ");
    const auto border = [&](std::string_view l, std::string_view r, std::string_view label) {
      std::string out(l);
      std::size_t n = 0;
      if (columns(label) + 4 <= w) out += "─" + std::string(label), n = 1 + columns(label);
      for (; n + 2 < w; ++n) out += "─";
      return paint("1", out + std::string(r));
    };
    row(top - 1, border("┌", "┐", paint("1", title)));
    for (std::size_t i = 0; i < h; ++i) row(top + i, paint("1", "│") + " " + fit(help[help_scroll_ + i], inner) + " " + paint("1", "│"));
    row(top + h, border("└", "┘", ""));
  }
  // What a line shows from column `from` on, its attributes as they were there.
  static std::string slice_after(std::string_view s, std::size_t from) {
    std::string out, sgr;
    std::size_t col = 0;
    for (std::size_t i = 0; i < s.size(); ++i) {
      if (s[i] == '\x1b') {
        const std::size_t j = s.find('m', i);
        const std::string_view seq = s.substr(i, j == std::string_view::npos ? s.size() - i : j - i + 1);
        if (col >= from) out += seq;
        else sgr = seq == "\x1b[0m" ? std::string() : sgr + std::string(seq);
        i = j == std::string_view::npos ? s.size() : j;
        continue;
      }
      const bool lead = (static_cast<unsigned char>(s[i]) & 0xc0) != 0x80;
      if (lead) ++col;
      if (col > from) out += s[i];
    }
    return sgr + out;
  }

  struct Press {
    Hit hit;
    std::chrono::milliseconds time;
    std::int32_t node;  // the row's node: a live feed that moved the rows breaks a double click
  };
  // The node of a row or marker hit; -1 for anything else.
  std::int32_t row_node(const Hit& h) const {
    if (h.area != Hit::Area::row && h.area != Hit::Area::marker) return -1;
    const auto rows = tree_.rows();
    return h.index < rows.size() ? rows[h.index].node : -1;
  }
  bool pane_focused() const { return focus_ == Focus::detail && split(); }

  Tree& tree_;
  std::size_t rows_ = 24, cols_ = 80;
  std::optional<std::string> live_;
  std::optional<std::string> ended_;  // what the title bar says once the source has ended
  bool follow_ = false;               // the newest activity is selected as it comes (#939)
  View view_ = View::tree;
  Focus focus_ = Focus::tree;
  std::size_t cursor_ = 0, top_ = 0;
  std::size_t lane_ = 0, time_ = 0;
  std::size_t detail_scroll_ = 0;
  bool detail_ = true, help_ = false, prompt_ = false;
  mutable std::size_t help_scroll_ = 0;  // clamped as the help is drawn
  std::string filter_, typed_;
  std::size_t split_ = 600;  // the divider's place, in thousandths of the width
  Drag drag_ = Drag::none;
  bool moved_ = false;  // the pointer moved since the press: a drag, not a click
  std::optional<RowSel> rows_sel_;
  std::optional<TextSel> text_sel_;
  bool visual_ = false;  // V: the keys extend the selection
  bool show_source_ = false;  // the source column (S)
  std::optional<Mouse> last_drag_;  // where the pointer was last seen in a drag
  // The histogram (#637): its metrics shown (self time, time in all, allocated and runs at first),
  // grouping, scope, order, and the selected bar.
  std::array<bool, metric_count> metrics_on_{true, true, true, false, false, false, true, false, false};
  Grouping group_by_ = Grouping::kind;
  bool within_ = false;
  Metric sort_by_ = Metric::self_time;
  std::chrono::milliseconds opened_at_{-1'000'000};  // when a bar was opened: a double click's second press is ignored
  std::size_t bar_ = 0, bars_top_ = 0;
  bool past_edge_ = false;          // and that was past the edge of what it selects in
  std::string notice_;   // `copied …`, until the next input
  std::optional<Press> last_press_;
};

}  // namespace ycxx::trace
