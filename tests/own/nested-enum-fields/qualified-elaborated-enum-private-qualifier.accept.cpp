// #761 [dcl.type.elab], [basic.lookup.elab], [class.access.general].
struct StorageType { enum class State { ready }; };

class Register {
  using Storage = StorageType;

public:
  static Storage::State read();
};

enum Register::Storage::State Register::read() {
  return Storage::State::ready;
}
