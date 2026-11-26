import std;
import ycxx.engine;

extern "C" void embedded_peer_throw();

int main() {
  ycxx::engine::Db db;
  try {
    embedded_peer_throw();
  } catch (const std::logic_error& error) {
    return std::current_exception() && std::string_view(error.what()) == "stock peer exception" ? 0 : 1;
  } catch (...) {
    return 2;
  }
  return 3;
}
