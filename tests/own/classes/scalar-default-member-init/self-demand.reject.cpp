struct Settings {
  int earlier;
  int limit = Settings{7}.earlier; //! error
};

int demand() {
  Settings local{};
  return local.limit;
}
