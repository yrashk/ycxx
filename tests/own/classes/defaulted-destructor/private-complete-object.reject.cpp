// #157 [class.dtor]: a private destructor must be accessible at an automatic object's declaration.
struct Hidden {
private:
  ~Hidden() = default;
};

void create_hidden() {
  Hidden object; //! error
}
