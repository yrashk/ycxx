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
      mouse(*m, now);
      return {};
    }
    const std::string& k = std::get<Key>(in).seq;
    if (k.empty()) return {};
    if (prompt_) {
      edit_filter(k);
      return {};
    }
    if (help_) {
      help_ = false;
      return {};
    }
    if (k == "q" || k == "\x03") return {true};
    if (view_ == View::tree && pane_focused() && detail_key(k)) return {};
    if (view_ == View::tree) tree_key(k);
    else timeline_key(k);
    common_key(k);
    return {};
  }

  Frame frame() {
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
  bool dragging() const noexcept { return dragging_; }
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
  void mouse(const Mouse& m, std::chrono::milliseconds now) {
    if (m.act == Mouse::Act::move) return;
    if (m.act == Mouse::Act::drag) {
      if (dragging_ && split()) place_divider(m.x);
      return;
    }
    if (m.act == Mouse::Act::release) {
      dragging_ = false;
      return;
    }
    if (help_) {
      if (m.act == Mouse::Act::press) help_ = false;
      return;
    }
    const Hit h = hit(m.x, m.y);
    if (m.act == Mouse::Act::wheel_up || m.act == Mouse::Act::wheel_down) {
      wheel(h, m.act == Mouse::Act::wheel_down ? 1 : -1, m.shift);
      return;
    }
    if (m.button != 0) return;
    // Twice on the same thing (row, slice), not merely the same line.
    const std::int32_t node = row_node(h);
    const bool twice = last_press_ && now - last_press_->time <= double_click && last_press_->hit == h && last_press_->node == node;
    last_press_ = twice ? std::nullopt : std::optional(Press{h, now, node});
    dragging_ = h.area == Hit::Area::divider;  // a release that never came ends here
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
        break;
      case Hit::Area::marker:
        focus_ = Focus::tree;
        if (cursor_ != h.index) detail_scroll_ = 0;
        cursor_ = h.index;
        tree_.toggle(tree_.rows()[cursor_].node);
        last_press_.reset();  // a second click on the marker folds it back, not twice
        break;
      case Hit::Area::divider:
        dragging_ = true;
        break;
      case Hit::Area::title:
      case Hit::Area::detail:
        focus_ = Focus::detail;
        break;
      case Hit::Area::lane:
        lane_ = h.index;
        time_ = h.at;
        if (twice) jump_to_slice();
        break;
      default:
        break;
    }
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
    const std::optional<std::int32_t> held = selected_node();
    tree_.set_order(o);
    if (held) keep_selected(*held);
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
    else if (k == "h" || k == "\x1b[D" || k == "\x1bOD" || k == "\x1b") focus_ = Focus::tree;
    else return false;
    return true;
  }
  void common_key(const std::string& k) {
    if (k == "\t" || k == "t") view_ = view_ == View::tree ? View::timeline : View::tree;
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
      const std::optional<std::int32_t> held = selected_node();
      tree_.set_hide_reused(!tree_.hide_reused());
      if (held) keep_selected(*held);
    } else if (k == "s") {
      // Start order, then most allocated in the subtree, most retained, longest first (#274).
      reorder(static_cast<Tree::Order>((std::to_underlying(tree_.order()) + 1) % Tree::order_count));
    } else if (k == "o") {
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
    else if (k == "\r" || k == " ") tree_.toggle(rows[cursor_].node);
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
    return out;
  }
  std::string footer() const {
    if (view_ == View::tree && pane_focused())
      return paint("2", " pane: ↑↓/jk line  PgUp/PgDn page  g/G top/end  ←/h/Esc back to the tree  mouse: wheel scrolls, click the tree  ? help  q quit");
    if (view_ == View::tree)
      return paint("2", " ↑↓/jk move  ⏎/space fold  ←→ close/open  e/E all  n next run  x next error  r reused  s sort  shift+J/K ]/[ pane  </> resize  / filter  w/0-9/m/a worker  o order  v pane  t timeline  mouse: click, wheel, drag  ? help  q quit");
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
    for (std::size_t i = top_; i < rows.size() && left.size() <= hgt; ++i) {
      std::string line = tree_row(rows[i], spans);
      if (i == cursor_) line = selected(fit(line, lw));
      left.push_back(std::move(line));
    }
    if (rows.empty()) left.push_back(paint("2", "  nothing matches"));
    if (!split()) return left;
    const std::vector<std::string> right = rows.empty() ? std::vector<std::string>{} : detail(tree_.node(rows[cursor_].node));
    const std::size_t rw = cols_ - lw - 1;
    // A pane longer than the screen keeps its last line for where it is and how to move.
    const bool long_pane = right.size() > hgt && hgt > 1;
    const std::size_t shown = long_pane ? hgt - 1 : hgt;
    detail_scroll_ = right.size() > shown ? std::min(detail_scroll_, right.size() - shown) : 0;
    std::vector<std::string> out;
    const std::string_view title_sgr = pane_focused() ? "1;4" : "1";
    out.push_back(fit(left[0], lw) + paint(dragging_ ? "1;7" : "2", "⇔") + paint(title_sgr, fit(" cell", rw)));
    for (std::size_t i = 0; i < hgt; ++i) {
      std::string l = i + 1 < left.size() ? left[i + 1] : std::string();
      const std::size_t at = i + detail_scroll_;
      std::string r = at < right.size() ? right[at] : std::string();
      if (long_pane && i == hgt - 1)
        r = paint("2;7", std::format(" lines {}–{} of {}   shift+J/K line  ]/[ page  }}/{{ end/top ", detail_scroll_ + 1, detail_scroll_ + shown,
                                     right.size()));
      out.push_back(fit(l, lw) + paint(dragging_ ? "1;7" : "2", "│") + fit(r, rw));
    }
    return out;
  }

  std::vector<std::string> detail(const Tree::Node& n) const {
    const Event& e = n.ev;
    const std::size_t rw = cols_ - left_width() - 2;
    std::vector<std::string> out;
    out.push_back(" " + paint("1", e.kind));
    for (const std::string& l : wrap(e.key.empty() ? "(no key)" : e.key, rw)) out.push_back(" " + l);
    out.emplace_back();
    out.push_back(" " + paint(event_sgr(e), std::format("{} {}", outcome_glyph(e.outcome, true), outcome_name(e.outcome))));
    for (const std::string& l : wrap(outcome_meaning(e.outcome), rw)) out.push_back(" " + paint("2", l));
    out.emplace_back();
    out.push_back(std::format(" started  {}", format_duration(e.start_ns)));
    out.push_back(std::format(" took     {}", e.end_ns == e.start_ns ? std::string("(instant)") : format_duration(e.end_ns - e.start_ns)));
    out.push_back(std::format(" worker   {}", format_workers(e)));
    out.push_back(std::format(" depth    {}", e.depth));
    out.push_back(std::format(" below    {} cells, {} ran", n.subtree - 1, ran_below(n)));
    if (n.parent >= 0) out.push_back(" for      " + paint("2", cell_name(tree_.node(n.parent).ev)));
    out.emplace_back();
    // Memory (#274): this frame's own, its subtree's, and what the cell retains.
    if (e.alloc_bytes > 0 || n.incl.alloc > 0 || e.retained_bytes != 0) {
      out.push_back(" " + paint("1;4", "memory"));
      out.push_back(std::format(" alloc    {} in {} allocations", format_bytes(static_cast<std::int64_t>(e.alloc_bytes)), e.allocs));
      out.push_back(std::format(" peak     {}", format_bytes(e.peak_bytes)));
      out.push_back(std::format(" retains  {}", format_bytes(e.retained_bytes)));
      out.push_back(std::format(" in all   {} allocated, {} retained", format_bytes(static_cast<std::int64_t>(n.incl.alloc)), format_bytes(n.incl.retained)));
      out.emplace_back();
    }
    if (!e.errors.empty()) {
      out.push_back(" " + paint("1;4;31", "errors"));
      for (const std::string& l : wrap(e.errors, rw)) out.push_back(" " + paint(error_sgr, l));
      out.emplace_back();
    }
    if (!e.value.empty()) {
      out.push_back(" " + paint("1;4", "value"));
      for (const std::string& l : wrap(e.value, rw)) out.push_back(" " + l);
    } else {
      out.push_back(paint("2", " no value shown for this kind"));
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
  bool dragging_ = false;
  std::optional<Press> last_press_;
};

}  // namespace ycxx::trace
