struct Buffer { char text[16]{}; unsigned long length = 0; };
consteval Buffer make_buffer() {
  Buffer data;
  return data;
}
static_assert(make_buffer().length == 0);
