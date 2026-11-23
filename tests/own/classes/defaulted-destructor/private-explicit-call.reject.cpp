// #157 [class.dtor]: access is checked when a destructor is called explicitly.
struct Hidden {
private:
  ~Hidden() = default;
};

void destroy_hidden(Hidden* pointer) {
  pointer->~Hidden(); //! error
}
