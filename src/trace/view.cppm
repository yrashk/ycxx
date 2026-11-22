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
// The selected row: reverse video, kept across the resets inside it.
std::string selected(std::string_view line) {
  std::string out = "\x1b[7m";
  for (std::size_t i = 0; i < line.size(); ++i) {
    if (line.substr(i).starts_with("\x1b[0m")) {
      out += "\x1b[0;7m";
      i += 3;
    } else {
      out += line[i];
    }
  }
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

// Splits text into lines of at most `width` columns.
std::vector<std::string> wrap(std::string_view text, std::size_t width) {
  std::vector<std::string> out;
  for (auto part : text | std::views::split('\n')) {
    std::string_view line(part.begin(), part.end());
    do {
      std::size_t cut = 0, n = 0;
      while (cut < line.size() && n < width) {
        ++cut;
        while (cut < line.size() && (static_cast<unsigned char>(line[cut]) & 0xc0) == 0x80) ++cut;
        ++n;
      }
      out.emplace_back(line.substr(0, cut));
      line.remove_prefix(cut);
    } while (!line.empty());
  }
  return out;
}

enum class View : std::uint8_t { tree, timeline };
// Which pane the keys move: the tree, or the pane on its right (after a click on it).
enum class Focus : std::uint8_t { tree, detail };
// The tree pane's columns. A click on a header sorts by it (`cell`: as they started); `worker`
// does not sort.
enum class Column : std::uint8_t { cell, time, worker, alloc, retained };

// Where a point on the screen is, as the last frame drew it.
struct Hit {
  enum class Area : std::uint8_t {
    none,
    header,   // the summary line
    tab,      // a view's tab: `index` is the `View`
    status,   // the rest of the status line
    column,   // a column header: `index` is the `Column`
    row,      // a tree row: `index` in `Tree::rows()`
    marker,   // a tree row's ▸/▾: `index` as for `row`
    divider,  // between the tree and the pane
    title,    // the pane's title line
    detail,   // a line of the pane: `index` among its lines, scrolling included
    lane,     // a timeline lane: `index` the lane, `at` the slice
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
    rows_ = std::max<std::size_t>(rows, 5);
    cols_ = std::max<std::size_t>(cols, 20);
  }
  // A live source (`ctl trace`, `--watch`): the latest request on top, and the source named.
  void set_live(std::string source) {
    live_ = std::move(source);
    tree_.set_newest_first(true);
  }
  // Runs `pump` (more events into the tree), keeping the selected node selected unless the view
  // is at the top, where it follows what comes; false once the source has ended.
  bool pump(const std::function<bool()>& f) {
    const auto before = tree_.rows();
    const std::optional<std::int32_t> held = cursor_ > 0 && cursor_ < before.size() ? std::optional(before[cursor_].node) : std::nullopt;
    const bool more = f();
    if (!more) live_.reset(), ended_ = true;
    if (held) keep_selected(*held);
    return more;
  }

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
      help_ = false;
      return {};
    }
    if (k == "q" || k == "\x03") return Action{.quit = true};
    if (view_ == View::tree) {
      if (std::optional<Action> a = selection_key(k)) return *a;
    }
    if (view_ == View::tree && pane_focused() && detail_key(k)) return {};
    if (view_ == View::tree) tree_key(k);
    else timeline_key(k);
    common_key(k);
    // V in the tree: the keys move its end.
    if (visual_ && rows_sel_)
      if (const auto n = selected_node()) rows_sel_->end = *n;
    return {};
  }
  // What the last copy was, for the status line.
  void note(std::string text) { notice_ = std::move(text); }
  const std::string& notice() const noexcept { return notice_; }

  // While a drag is held past the edge of the tree or the pane, it keeps scrolling: the terminal
  // calls `tick` this often, as terminals report the pointer only when it moves.
  bool ticking() const noexcept { return (drag_ == Drag::rows || drag_ == Drag::text) && past_edge_ && last_drag_; }
  void tick() {
    if (ticking()) drag_to(*last_drag_);
  }
  static constexpr std::chrono::milliseconds tick_every{60};

  Frame frame() {
    drop_stale_selection();
    Frame f;
    f.lines = header_lines();
    f.lines.push_back(status_line());
    std::vector<std::string> body = view_ == View::tree ? tree_body() : timeline_body();
    body.resize(body_height());
    f.lines.append_range(body);
    f.lines.push_back(footer());
    if (help_) overlay_help(f.lines);
    for (std::string& l : f.lines) l = fit(l, cols_);
    while (f.lines.size() < rows_) f.lines.push_back(fit("", cols_));
    f.lines.resize(rows_);
    if (prompt_) f.cursor = std::pair<std::size_t, std::size_t>{header_height(), std::min(cols_ - 1, 9 + columns(typed_))};
    return f;
  }

  // What is at column `x`, row `y` of the last frame.
  Hit hit(std::size_t x, std::size_t y) const {
    const std::size_t hh = header_height();
    if (y < hh) return {Hit::Area::header};
    if (y == hh) {
      for (const Tab& t : tabs()) {
        if (x >= t.x0 && x < t.x1) return {Hit::Area::tab, std::to_underlying(t.view)};
      }
      return {Hit::Area::status};
    }
    if (y >= hh + 1 + body_height()) return {Hit::Area::footer};
    const std::size_t by = y - hh - 1;
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
    if (x >= indent && x < indent + 2 && indent + 2 <= column_spans()[0].x1 && !tree_.node(rows[row].node).children.empty())
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
  // Lines the summary takes: it wraps rather than run off a narrow screen.
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
      const std::size_t n = rows.size(), first = header_height() + 2, hgt = list_height();
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
      const std::size_t first = header_height() + 2;
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
        case Column::worker: return std::nullopt;
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
    const std::size_t lines = pane_lines().size(), first = header_height() + 2, shown = pane_shown(lines);
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
    if (k == "\t" || k == "t") view_ = view_ == View::tree ? View::timeline : View::tree, clear_selection();
    else if (k == "/") prompt_ = true, typed_ = filter_;
    else if (k == "?") help_ = true;
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
  // The next cell that ran (executed or cut off), opening the tree to it.
  void next_execution(const std::vector<Tree::Row>& rows) {
    const auto nodes = tree_.nodes();
    const std::int32_t from = rows[cursor_].node;
    for (std::size_t i = static_cast<std::size_t>(from) + 1; i < nodes.size(); ++i) {
      const Outcome o = nodes[i].ev.outcome;
      if ((o == Outcome::executed || o == Outcome::cutoff) && tree_.matches(nodes[i].ev)) {
        select(static_cast<std::int32_t>(i));
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
    const auto nodes = tree_.nodes();
    const auto from = static_cast<std::size_t>(rows[cursor_].node);
    for (std::size_t k = 1; k <= nodes.size(); ++k) {
      const auto i = static_cast<std::int32_t>((from + k) % nodes.size());
      if (tree_.error_here(i) && tree_.matches(nodes[static_cast<std::size_t>(i)].ev)) {
        select(i);
        return;
      }
    }
  }
  void select(std::int32_t node) {
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
  // The body: what the summary, the status line and the footer leave.
  std::size_t body_height() const {
    const std::size_t used = header_height() + 2;
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
  std::vector<Tab> tabs() const {
    std::vector<Tab> out;
    std::size_t x = 1;
    for (const auto& [v, name] : {std::pair{View::tree, std::string_view("tree")}, std::pair{View::timeline, std::string_view("timeline")}}) {
      out.push_back({v, name, x, x + name.size()});
      x += name.size() + 2;
    }
    return out;
  }

  // The tree pane's metric columns, right of the cell column, and their widths. Narrow panes drop
  // them from the right, so the cell keeps `min_cell` columns.
  static constexpr std::size_t min_cell = 24;
  static constexpr std::array<std::pair<Column, std::size_t>, 4> metrics{
      {{Column::time, 8}, {Column::worker, 7}, {Column::alloc, 9}, {Column::retained, 9}}};
  static std::string_view column_title(Column c) {
    switch (c) {
      case Column::cell: return "cell";
      case Column::time: return "time";
      case Column::worker: return "worker";
      case Column::alloc: return "alloc";
      case Column::retained: return "retains";
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
    for (const auto& [c, w] : metrics) {
      if (lw < min_cell + used + w + 1) break;
      used += w + 1;
      ++shown;
    }
    std::vector<Span> out{{Column::cell, 0, lw - used}};
    std::size_t x = lw - used;
    for (std::size_t i = 0; i < shown; ++i) {
      out.push_back({metrics[i].first, x + 1, x + 1 + metrics[i].second});
      x += metrics[i].second + 1;
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
  // The summary, in pieces that wrap onto as many lines as the width needs (at most a third of
  // the screen): the title, each outcome's count, events and time, and memory.
  std::vector<std::string> header_lines() const {
    const Summary s = tree_.summary();
    std::vector<std::string> pieces{paint("1;7", " ycxx engine trace ")};
    for (std::uint8_t i = 0; i < outcome_count; ++i) {
      const auto o = static_cast<Outcome>(i);
      if (s.counts[i] == 0) continue;
      pieces.push_back(paint(outcome_sgr(o), std::format("{} {} {}", outcome_glyph(o, true), s.counts[i], outcome_name(o))));
    }
    pieces.push_back(paint("2", std::format("{} events, {} workers, {}", s.events, s.workers, format_duration(s.span_ns))));
    // Memory (#274): what the frames allocated and the cells retain; the process's, when known.
    pieces.push_back(paint("2", std::format("{} allocated, {} retained", format_bytes(static_cast<std::int64_t>(s.alloc_bytes)), format_bytes(s.retained_bytes))));
    if (const auto m = tree_.memory())
      pieces.push_back(paint("2", std::format("last build: process {}, peak {}", format_bytes(m->process_current), format_bytes(m->process_peak))));
    std::vector<std::string> out{pieces[0]};
    for (std::size_t i = 1; i < pieces.size(); ++i) {
      if (columns(out.back()) + 2 + columns(pieces[i]) <= cols_) out.back() += "  " + pieces[i];
      else out.push_back(" " + pieces[i]);
    }
    const std::size_t most = std::max<std::size_t>(1, rows_ / 3);
    if (out.size() > most) out.resize(most);
    return out;
  }
  std::string status_line() const {
    if (prompt_) return std::format(" filter: {}", typed_);
    std::string out = " ";
    for (const Tab& t : tabs()) out += (view_ == t.view ? paint("1;4", t.name) : paint("2", t.name)) + "  ";
    if (!filter_.empty()) out += std::format(" filter: {}", paint("1", filter_));
    out += std::format("  worker: {}", tree_.worker() ? paint("1", format_worker(*tree_.worker())) : std::string("all"));
    out += tree_.newest_first() ? "  newest first" : "  oldest first";
    if (tree_.order() == Tree::Order::alloc) out += "  " + paint("1", "by bytes allocated");
    else if (tree_.order() == Tree::Order::retained) out += "  " + paint("1", "by bytes retained");
    else if (tree_.order() == Tree::Order::time) out += "  " + paint("1", "by time");
    if (tree_.hide_reused()) out += "  " + paint("1", "reused hidden");
    if (live_) out += "  " + paint("1;38;5;208", "● live") + paint("2", " " + *live_);
    else if (ended_) out += "  " + paint("2", "(the daemon has gone)");
    if (visual_) out += "  " + paint("1;7", " V: j/k extend, y copy, Esc drop ");
    if (!notice_.empty()) out += "  " + paint("1;32", notice_);
    return out;
  }
  std::string footer() const {
    if (view_ == View::tree && pane_focused())
      return paint("2", " pane: ↑↓/jk line  PgUp/PgDn page  g/G top/end  V select lines  y copy  ←/h/Esc back to the tree  mouse: drag selects and copies, wheel scrolls  ? help  q quit");
    if (view_ == View::tree)
      return paint("2", " ↑↓/jk move  ⏎/space fold  ←→ close/open  e/E all  n next run  x next error  r reused  s sort  shift+J/K ]/[ pane  </> resize  / filter  w/0-9/m/a worker  o order  v pane  t timeline  y/Y copy cell/subtree  V select  mouse: click, wheel, drag  ? help  q quit");
    return paint("2", " ↑↓ lane  ←→/HL scrub  ⏎ open in tree  w/0-9/m/a worker  / filter  t tree  mouse: click a slice, double-click opens it, wheel scrubs  ? help  q quit");
  }

  std::string metric(const Tree::Node& n, Column c) const {
    const Event& e = n.ev;
    switch (c) {
      case Column::time: return e.end_ns == e.start_ns ? std::string() : format_duration(e.end_ns - e.start_ns);
      case Column::worker: return n.group ? std::string() : format_workers(e);
      case Column::alloc: return n.incl.alloc > 0 ? format_bytes(static_cast<std::int64_t>(n.incl.alloc)) : std::string();
      case Column::retained: return n.incl.retained > 0 ? format_bytes(n.incl.retained) : std::string();
      case Column::cell: return {};
    }
    return {};
  }
  // The cell column: indentation, the marker, the outcome's glyph, the cell, its outcome, and what
  // is folded under it.
  std::string cell_text(const Tree::Row& r) const {
    const Tree::Node& n = tree_.node(r.node);
    const Event& e = n.ev;
    std::string out(2 * r.depth, ' ');
    out += n.children.empty() ? "  " : (n.expanded ? "▾ " : "▸ ");
    if (tree_.error_here(r.node)) {
      // Where the errors start: the whole cell on red, and the first message on it.
      return out + paint(error_here_sgr, std::format("{} {} {} ← {}", outcome_glyph(e.outcome, true), cell_name(e), outcome_name(e.outcome),
                                                     first_line(e.errors)));
    }
    out += paint(event_sgr(e), outcome_glyph(e.outcome, true));
    out += ' ';
    out += std::format("{}{}", paint(r.match ? "1" : "2", e.kind), e.key.empty() ? "" : std::format("({})", e.key));
    out += ' ' + paint(event_sgr(e), outcome_name(e.outcome));
    if (e.repeat > 1) out += paint("2", std::format(" ×{}", e.repeat));
    if (!e.errors.empty()) {
      // The message from where it starts, bubbled up; `x` goes there.
      const auto at = tree_.error_origin_below(r.node);
      out += paint(error_sgr, std::format("  (error below: {})", first_line(at ? tree_.node(*at).ev.errors : e.errors)));
    }
    if (!n.expanded && n.children.size() > 0) out += paint("2", std::format("  [{} below, {} ran]", n.subtree - 1, ran_below(n)));
    return out;
  }
  std::string tree_row(const Tree::Row& r, const std::vector<Span>& spans) const {
    const Tree::Node& n = tree_.node(r.node);
    std::string out = fit(cell_text(r), spans[0].x1);
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
    out.push_back(fit(left[0], lw) + paint(dragging() ? "1;7" : "2", "⇔") + paint(title_sgr, fit(" cell", rw)));
    for (std::size_t i = 0; i < hgt; ++i) {
      std::string l = i + 1 < left.size() ? left[i + 1] : std::string();
      const std::size_t at = i + detail_scroll_;
      std::string r = at < right.size() ? right[at].text : std::string();
      if (text_sel_ && at < right.size()) {
        const auto [from, to] = covered(*text_sel_, at, columns(r));
        if (from < to) r = highlight(r, from, to);
      }
      if (long_pane && i == hgt - 1)
        r = paint("2;7", std::format(" lines {}–{} of {}   shift+J/K line  ]/[ page  }}/{{ end/top ", detail_scroll_ + 1, detail_scroll_ + shown,
                                     right.size()));
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
    wrapped(std::format("{} {}", outcome_glyph(e.outcome, true), outcome_name(e.outcome)), event_sgr(e));
    wrapped(outcome_meaning(e.outcome), "2");
    add("");
    add(std::format("started  {}", format_duration(e.start_ns)));
    add(std::format("took     {}", e.end_ns == e.start_ns ? std::string("(instant)") : format_duration(e.end_ns - e.start_ns)));
    add(std::format("worker   {}", format_workers(e)));
    add(std::format("depth    {}", e.depth));
    add(std::format("below    {} cells, {} ran", n.subtree - 1, ran_below(n)));
    if (n.parent >= 0) add("for      " + cell_name(tree_.node(n.parent).ev));
    add("");
    // Memory (#274): this frame's own, its subtree's, and what the cell retains.
    if (e.alloc_bytes > 0 || n.incl.alloc > 0 || e.retained_bytes != 0) {
      wrapped("memory", "1;4");
      add(std::format("alloc    {} in {} allocations", format_bytes(static_cast<std::int64_t>(e.alloc_bytes)), e.allocs));
      add(std::format("peak     {}", format_bytes(e.peak_bytes)));
      add(std::format("retains  {}", format_bytes(e.retained_bytes)));
      add(std::format("in all   {} allocated, {} retained", format_bytes(static_cast<std::int64_t>(n.incl.alloc)), format_bytes(n.incl.retained)));
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

  std::vector<std::string> timeline_body() {
    const std::size_t cols = lane_columns();
    time_ = std::min(time_, cols - 1);
    const Timeline tl = timeline(tree_, cols);
    lane_ = std::min(lane_, tl.lanes.size() - 1);
    std::vector<std::string> out;
    const auto [from, to] = tl.slice(time_, cols);
    // The axis: 0, the cursor's time, the end.
    out.push_back(paint("2", std::format(" {:<7}0{:>{}}", "", format_duration(tl.span_ns), cols - 1)));
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
    for (std::size_t lane = 0; lane < tl.lanes.size(); ++lane) {
      const std::int32_t node = tl.lanes[lane][time_];
      std::string line = std::format(" {}{:>6}  ", lane == lane_ ? "▶" : " ", format_worker(Timeline::worker_of(lane)));
      if (node < 0) {
        line += paint("2", "idle");
      } else {
        // The chain from the top-level request down to the cell, so a slice says why.
        std::vector<std::string> chain;
        for (std::int32_t p = node; p >= 0; p = tree_.node(p).parent) chain.push_back(tree_.node(p).ev.kind);
        std::ranges::reverse(chain);
        const Event& e = tree_.node(node).ev;
        line += paint(event_sgr(e), outcome_glyph(e.outcome, true)) + " " + cell_name(e) + " " + paint(event_sgr(e), outcome_name(e.outcome)) +
                paint("2", "   " + (chain | std::views::join_with(std::string_view(" › ")) | std::ranges::to<std::string>()));
      }
      out.push_back(std::move(line));
    }
    out.emplace_back();
    std::string legend = " ";
    for (std::uint8_t i = 0; i < outcome_count; ++i) {
      const auto o = static_cast<Outcome>(i);
      legend += paint(outcome_sgr(o), "█") + " " + std::string(outcome_name(o)) + "  ";
    }
    out.push_back(legend + paint("2", "█▓▒░ shallower to deeper"));
    return out;
  }

  void overlay_help(std::vector<std::string>& lines) const {
    std::vector<std::string> help = {
        paint("1", " What the engine did with each cell"),
        "",
    };
    for (std::uint8_t i = 0; i < outcome_count; ++i) {
      const auto o = static_cast<Outcome>(i);
      help.push_back(std::format("  {} {:<9} {}", paint(outcome_sgr(o), outcome_glyph(o, true)), outcome_name(o), outcome_meaning(o)));
    }
    help.insert(help.end(), {
                                "",
                                paint("1", " Filter (/)"),
                                "  words      match kind or key, all of them",
                                "  is:exec    an outcome (any prefix)",
                                "  w:3 w:main a worker",
                                "",
                                paint("1", " Showing"),
                                "  r hides reused cells (what ran stays), o flips the order",
                                "  s sorts siblings: as they started, by bytes allocated below, by bytes retained, by time",
                                "  the alloc and retains columns sum over each cell's subtree",
                                "",
                                paint("1", " The pane on the right"),
                                "  shift+J / shift+K a line (j/k move in the tree), ]/[ a page, }/{ end and top",
                                "  < and > move the divider (or drag it); v hides and shows the pane",
                                "",
                                paint("1", " Workers"),
                                "  w cycles, 0-9 picks, m the requesting thread, a all",
                                "",
                                paint("1", " Copy (to the clipboard: OSC 52, and pbcopy, wl-copy or xclip when local)"),
                                "  y the cell (what the pane shows), Y its subtree as the stream prints it",
                                "  drag over rows or the pane's text, or V and j/k then y: what is selected",
                                "",
                                paint("1", " Mouse"),
                                "  click a row to select it, its ▸ or a double click to fold it; click a column header to sort",
                                "  click a tab or a pane to switch to it; the wheel scrolls what is under it; drag the │ divider",
                                "",
                                paint("2", " any key or click to close"),
                            });
    const std::size_t top = 3;
    for (std::size_t i = 0; i < help.size() && top + i < lines.size(); ++i) lines[top + i] = "  " + help[i];
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
  bool ended_ = false;
  View view_ = View::tree;
  Focus focus_ = Focus::tree;
  std::size_t cursor_ = 0, top_ = 0;
  std::size_t lane_ = 0, time_ = 0;
  std::size_t detail_scroll_ = 0;
  bool detail_ = true, help_ = false, prompt_ = false;
  std::string filter_, typed_;
  std::size_t split_ = 600;  // the divider's place, in thousandths of the width
  Drag drag_ = Drag::none;
  bool moved_ = false;  // the pointer moved since the press: a drag, not a click
  std::optional<RowSel> rows_sel_;
  std::optional<TextSel> text_sel_;
  bool visual_ = false;  // V: the keys extend the selection
  std::optional<Mouse> last_drag_;  // where the pointer was last seen in a drag
  bool past_edge_ = false;          // and that was past the edge of what it selects in
  std::string notice_;   // `copied …`, until the next input
  std::optional<Press> last_press_;
};

}  // namespace ycxx::trace
