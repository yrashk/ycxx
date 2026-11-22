struct Guard {
  int* destroyed;
  ~Guard() noexcept;
};
struct Held { Guard guard; };
Held hold(int* destroyed) noexcept;
