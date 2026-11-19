// The interactive trace browser (#245): a full-screen view of a finished trace on the terminal,
// in plain ANSI over termios.
//
// - The tree: what each request asked of the engine, nested by demand, with a pane on the right
//   for the selected cell (its outcome, times, worker, subtree, and its value when the kind can
//   show one).
// - The timeline: one lane per pool worker over the trace's span, each slice colored by the
//   outcome of the deepest cell the worker was in; a cursor scrubs through time and Enter jumps to
//   the cell under it in the tree.
// - Filters: `/` by kind, key, `is:<outcome>` or `w:<worker>`; `w` cycles one worker, 0–9 picks
//   one, `a` shows all again.
module;
#include <fcntl.h>
#include <poll.h>
#include <sys/ioctl.h>
#include <termios.h>
#include <unistd.h>
export module ycxx.trace:browse;

import std;
import :model;

export namespace ycxx::trace {

// A live source of events (`ycxx ctl trace`): when `fd` is readable, `pump` reads from it into the
// tree; false once the source has ended.
struct Feed {
  int fd = -1;
  std::function<bool()> pump;
};

// Whether `fd` is a terminal that may get colors: a TTY, and `NO_COLOR` not set (no-color.org).
bool colors_for(int fd) {
  const char* no = std::getenv("NO_COLOR");
  return ::isatty(fd) == 1 && !(no && *no);
}
bool is_terminal(int fd) { return ::isatty(fd) == 1; }

// Runs the browser on the controlling terminal until `q`; false (and nothing drawn) when there is
// no terminal to run it on. With a feed, the tree grows while it is browsed.
bool browse(Tree& tree, Feed* feed = nullptr);

}  // namespace ycxx::trace

namespace ycxx::trace {
namespace {

// The terminal in raw mode on the alternate screen, restored when this goes away.
class Terminal {
 public:
  Terminal() {
    fd_ = ::open("/dev/tty", O_RDWR | O_CLOEXEC);
    if (fd_ < 0) return;
    if (::tcgetattr(fd_, &saved_) != 0) {
      ::close(fd_);
      fd_ = -1;
      return;
    }
    termios raw = saved_;
    raw.c_iflag &= ~static_cast<tcflag_t>(BRKINT | ICRNL | INPCK | ISTRIP | IXON);
    raw.c_oflag &= ~static_cast<tcflag_t>(OPOST);
    raw.c_cflag |= CS8;
    raw.c_lflag &= ~static_cast<tcflag_t>(ECHO | ICANON | IEXTEN | ISIG);
    raw.c_cc[VMIN] = 1;
    raw.c_cc[VTIME] = 0;
    ::tcsetattr(fd_, TCSAFLUSH, &raw);
    write("\x1b[?1049h\x1b[?25l");
  }
  ~Terminal() {
    if (fd_ < 0) return;
    write("\x1b[0m\x1b[?25h\x1b[?1049l");
    ::tcsetattr(fd_, TCSAFLUSH, &saved_);
    ::close(fd_);
  }
  Terminal(const Terminal&) = delete;

  explicit operator bool() const noexcept { return fd_ >= 0; }
  std::pair<std::size_t, std::size_t> size() const {
    winsize ws{};
    if (::ioctl(fd_, TIOCGWINSZ, &ws) != 0 || ws.ws_col == 0) return {24, 80};
    return {ws.ws_row, ws.ws_col};
  }
  void write(std::string_view s) const {
    while (!s.empty()) {
      const auto n = ::write(fd_, s.data(), s.size());
      if (n <= 0) return;
      s.remove_prefix(static_cast<std::size_t>(n));
    }
  }
  // One key: a byte, or an escape sequence as one string; empty when `feed` became readable first.
  std::string key(int feed = -1) const {
    if (feed >= 0) {
      pollfd fds[2] = {{fd_, POLLIN, 0}, {feed, POLLIN, 0}};
      while (::poll(fds, 2, -1) < 0) {}
      if (!(fds[0].revents & POLLIN) && fds[1].revents) return {};
    }
    char c = 0;
    if (::read(fd_, &c, 1) != 1) return "q";
    if (c != '\x1b') return std::string(1, c);
    std::string seq(1, c);
    // The rest of a sequence arrives at once; a lone Escape does not.
    termios t{};
    ::tcgetattr(fd_, &t);
    termios quick = t;
    quick.c_cc[VMIN] = 0;
    quick.c_cc[VTIME] = 1;
    ::tcsetattr(fd_, TCSANOW, &quick);
    while (seq.size() < 8) {
      char n = 0;
      if (::read(fd_, &n, 1) != 1) break;
      seq += n;
      if (seq.size() > 2 && (std::isalpha(static_cast<unsigned char>(n)) || n == '~')) break;
    }
    ::tcsetattr(fd_, TCSANOW, &t);
    return seq;
  }

 private:
  int fd_ = -1;
  termios saved_{};
};

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

enum class View { tree, timeline };

class Browser {
 public:
  Browser(Tree& tree, Terminal& term, Feed* feed) : tree_(tree), term_(term), feed_(feed) {}

  void run() {
    while (true) {
      draw();
      const std::string k = term_.key(feed_ ? feed_->fd : -1);
      if (k.empty()) {
        // Live: new events; the selection stays where it was.
        if (!feed_->pump()) feed_ = nullptr, ended_ = true;
        continue;
      }
      if (prompt_) {
        edit_filter(k);
        continue;
      }
      if (help_) {
        help_ = false;
        continue;
      }
      if (k == "q" || k == "\x03") return;
      if (view_ == View::tree) tree_key(k);
      else timeline_key(k);
      common_key(k);
    }
  }

 private:
  // ---- keys
  void common_key(const std::string& k) {
    if (k == "\t" || k == "t") view_ = view_ == View::tree ? View::timeline : View::tree;
    else if (k == "/") prompt_ = true, typed_ = filter_;
    else if (k == "?") help_ = true;
    else if (k == "v") detail_ = !detail_;
    else if (k == "J") ++detail_scroll_;
    else if (k == "K") detail_scroll_ = detail_scroll_ > 0 ? detail_scroll_ - 1 : 0;
    else if (k == "w") cycle_worker();
    else if (k == "a") pick_worker(std::nullopt);
    else if (k.size() == 1 && std::isdigit(static_cast<unsigned char>(k[0]))) pick_worker(k[0] - '0');
    else if (k == "m") pick_worker(-1);
  }
  void tree_key(const std::string& k) {
    const auto rows = tree_.rows();
    const std::size_t page = body_height();
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
    if (k != "J" && k != "K") detail_scroll_ = 0;
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
  void select(std::int32_t node) {
    tree_.reveal(node);
    const auto rows = tree_.rows();
    for (std::size_t i = 0; i < rows.size(); ++i) {
      if (rows[i].node == node) cursor_ = i;
    }
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
    if (lane_ >= tl.lanes.size()) return;
    const std::int32_t node = tl.lanes[lane_][time_];
    if (node < 0) return;
    view_ = View::tree;
    select(node);
  }

  // ---- layout
  std::size_t height() const { return term_.size().first; }
  std::size_t width() const { return term_.size().second; }
  std::size_t body_height() const { return height() > 4 ? height() - 4 : 1; }
  bool split() const { return detail_ && width() >= 100; }
  std::size_t left_width() const { return split() ? width() * 3 / 5 : width(); }
  std::size_t lane_columns() const { return width() > 12 ? width() - 9 : 1; }

  // ---- drawing
  void draw() {
    const auto [h, w] = term_.size();
    std::string frame = "\x1b[H";
    std::vector<std::string> lines;
    lines.push_back(header());
    lines.push_back(status_line());
    std::vector<std::string> body = view_ == View::tree ? tree_body() : timeline_body();
    body.resize(body_height());
    lines.append_range(body);
    lines.push_back(footer());
    if (help_) overlay_help(lines);
    for (std::size_t i = 0; i < h && i < lines.size(); ++i) {
      frame += fit(lines[i], w);
      if (i + 1 < h) frame += "\r\n";
    }
    if (prompt_) frame += std::format("\x1b[{};{}H\x1b[?25h", 2, std::min(w, 10 + columns(typed_)));
    else frame += "\x1b[?25l";
    term_.write(frame);
  }

  std::string header() const {
    const Summary s = tree_.summary();
    std::string out = paint("1;7", " ycxx engine trace ");
    for (std::uint8_t i = 0; i < outcome_count; ++i) {
      const auto o = static_cast<Outcome>(i);
      if (s.counts[i] == 0) continue;
      out += "  " + paint(outcome_sgr(o), std::format("{} {} {}", outcome_glyph(o, true), s.counts[i], outcome_name(o)));
    }
    out += paint("2", std::format("   {} events, {} workers, {}", s.events, s.workers, format_duration(s.span_ns)));
    return out;
  }
  std::string status_line() const {
    const auto tab = [&](View v, std::string_view name) { return view_ == v ? paint("1;4", name) : paint("2", name); };
    std::string out = std::format(" {} {}  ", tab(View::tree, "tree"), tab(View::timeline, "timeline"));
    if (prompt_) return std::format(" filter: {}", typed_);
    if (!filter_.empty()) out += std::format(" filter: {}", paint("1", filter_));
    out += std::format("  worker: {}", tree_.worker() ? paint("1", format_worker(*tree_.worker())) : std::string("all"));
    if (feed_) out += "  " + paint("1;31", "● live");
    else if (ended_) out += "  " + paint("2", "(the daemon has gone)");
    return out;
  }
  std::string footer() const {
    if (view_ == View::tree)
      return paint("2", " ↑↓/jk move  ⏎/space fold  ←→ close/open  e/E all  n next run  / filter  w/0-9/m/a worker  v pane  t timeline  ? help  q quit");
    return paint("2", " ↑↓ lane  ←→/HL scrub  ⏎ open in tree  w/0-9/m/a worker  / filter  t tree  ? help  q quit");
  }

  std::string tree_row(const Tree::Row& r) const {
    const Tree::Node& n = tree_.node(r.node);
    const Event& e = n.ev;
    std::string out(2 * r.depth, ' ');
    out += n.children.empty() ? "  " : (n.expanded ? "▾ " : "▸ ");
    out += paint(outcome_sgr(e.outcome), outcome_glyph(e.outcome, true));
    out += ' ';
    const std::string name = std::format("{}{}", paint(r.match ? "1" : "2", e.kind), e.key.empty() ? "" : std::format("({})", e.key));
    out += name;
    out += ' ' + paint(outcome_sgr(e.outcome), outcome_name(e.outcome));
    if (e.end_ns != e.start_ns) out += paint("2", " " + format_duration(e.end_ns - e.start_ns));
    out += paint("2", " " + format_workers(e));
    if (!n.expanded && n.children.size() > 0) out += paint("2", std::format("  [{} below, {} ran]", n.subtree - 1, n.executions - (e.outcome == Outcome::executed || e.outcome == Outcome::cutoff)));
    return out;
  }

  std::vector<std::string> tree_body() {
    const auto rows = tree_.rows();
    const std::size_t hgt = body_height();
    if (!rows.empty()) cursor_ = std::min(cursor_, rows.size() - 1);
    else cursor_ = 0;
    if (cursor_ < top_) top_ = cursor_;
    if (cursor_ >= top_ + hgt) top_ = cursor_ - hgt + 1;
    std::vector<std::string> left;
    for (std::size_t i = top_; i < rows.size() && left.size() < hgt; ++i) {
      std::string line = tree_row(rows[i]);
      if (i == cursor_) line = selected(fit(line, left_width()));
      left.push_back(std::move(line));
    }
    if (rows.empty()) left.push_back(paint("2", "  nothing matches"));
    if (!split()) return left;
    const std::vector<std::string> right = rows.empty() ? std::vector<std::string>{} : detail(tree_.node(rows[cursor_].node));
    const std::size_t lw = left_width(), rw = width() - lw - 1;
    std::vector<std::string> out;
    for (std::size_t i = 0; i < hgt; ++i) {
      std::string l = i < left.size() ? left[i] : std::string();
      const std::size_t at = i + detail_scroll_;
      const std::string r = at < right.size() ? right[at] : std::string();
      out.push_back(fit(l, lw) + paint("2", "│") + fit(r, rw));
    }
    return out;
  }

  std::vector<std::string> detail(const Tree::Node& n) const {
    const Event& e = n.ev;
    const std::size_t rw = width() - left_width() - 2;
    std::vector<std::string> out;
    out.push_back(" " + paint("1", e.kind));
    for (const std::string& l : wrap(e.key.empty() ? "(no key)" : e.key, rw)) out.push_back(" " + l);
    out.emplace_back();
    out.push_back(" " + paint(outcome_sgr(e.outcome), std::format("{} {}", outcome_glyph(e.outcome, true), outcome_name(e.outcome))));
    for (const std::string& l : wrap(outcome_meaning(e.outcome), rw)) out.push_back(" " + paint("2", l));
    out.emplace_back();
    out.push_back(std::format(" started  {}", format_duration(e.start_ns)));
    out.push_back(std::format(" took     {}", e.end_ns == e.start_ns ? std::string("(instant)") : format_duration(e.end_ns - e.start_ns)));
    out.push_back(std::format(" worker   {}", format_workers(e)));
    out.push_back(std::format(" depth    {}", e.depth));
    out.push_back(std::format(" below    {} cells, {} ran", n.subtree - 1, n.executions - (e.outcome == Outcome::executed || e.outcome == Outcome::cutoff)));
    if (n.parent >= 0) out.push_back(" for      " + paint("2", cell_name(tree_.node(n.parent).ev)));
    out.emplace_back();
    if (!e.value.empty()) {
      out.push_back(" " + paint("1;4", "value") + paint("2", "  (J/K scroll)"));
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
        line += paint(std::format("{}{}", outcome_sgr(e.outcome), cur ? ";7" : ""), block);
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
        line += paint(outcome_sgr(e.outcome), outcome_glyph(e.outcome, true)) + " " + cell_name(e) + " " +
                paint(outcome_sgr(e.outcome), outcome_name(e.outcome)) + paint("2", "   " + (chain | std::views::join_with(std::string_view(" › ")) | std::ranges::to<std::string>()));
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
                                paint("1", " Workers"),
                                "  w cycles, 0-9 picks, m the requesting thread, a all",
                                "",
                                paint("2", " any key to close"),
                            });
    const std::size_t top = 3;
    for (std::size_t i = 0; i < help.size() && top + i < lines.size(); ++i) lines[top + i] = "  " + help[i];
  }

  Tree& tree_;
  Terminal& term_;
  Feed* feed_;
  bool ended_ = false;
  View view_ = View::tree;
  std::size_t cursor_ = 0, top_ = 0;
  std::size_t lane_ = 0, time_ = 0;
  std::size_t detail_scroll_ = 0;
  bool detail_ = true, help_ = false, prompt_ = false;
  std::string filter_, typed_;
};

}  // namespace

bool browse(Tree& tree, Feed* feed) {
  Terminal term;
  if (!term) return false;
  Browser(tree, term, feed).run();
  return true;
}

}  // namespace ycxx::trace
