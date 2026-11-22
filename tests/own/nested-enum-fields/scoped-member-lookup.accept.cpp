// [dcl.enum], [basic.scope.enum]
struct Box {
  enum class State { active = 17 };
  int active() const { return static_cast<int>(State::active); }
};

struct SameDeclaration {
  enum class State { active = 17 } active = State::active;
};

template<class T>
struct TemplateBox {
  enum class State { active };
  int active() const { return 17; }
};

int main() {
  return Box{}.active() != static_cast<int>(Box::State::active) ||
         TemplateBox<int>{}.active() != 17 ||
         SameDeclaration{}.active != SameDeclaration::State::active;
}
