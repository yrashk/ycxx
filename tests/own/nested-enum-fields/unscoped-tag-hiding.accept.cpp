// [dcl.enum], [basic.lookup.general], [basic.lookup.qual], [basic.lookup.elab]
struct Box {
  enum State { State = 7 };
  enum State value;
  int local() const { return State; }
};

static_assert(Box::State == 7);
static_assert(Box::State::State == 7);

struct TagLater {
  enum Mode { State = 9 };
  enum class State { available };
};

static_assert(TagLater::State == 9);
static_assert(static_cast<int>(TagLater::State::available) == 0);

template<class T>
struct TemplateBox {
  enum State { State = 11 };
  enum State value;
};

static_assert(TemplateBox<int>::State == 11);
static_assert(TemplateBox<int>::State::State == 11);

struct FriendFirst {
  friend int active(FriendFirst);
  enum State { active = 17 };
};

static_assert(FriendFirst::active == 17);

struct Entry {};
struct ElaboratedOuterTag {
  enum Mode { Entry };
  void use() { struct Entry value; }
};

int main() { return Box{Box::State}.local() != 7; }
