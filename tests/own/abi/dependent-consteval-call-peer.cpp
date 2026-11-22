// The peer needs the classifier to substitute the signature and only declares read_tag.
template<int Code>
struct Tag { int payload; };

template<class T>
consteval int code_for() { return sizeof(T); }

template<class T>
using TagFor = Tag<code_for<T>()>;

template<class T>
int read_tag(const TagFor<T>& tag);

int peer() {
  Tag<2> short_tag{31};
  Tag<5> long_tag{43};
  if (read_tag<char[2]>(short_tag) != 31) return 3;
  if (read_tag<char[5]>(long_tag) != 43) return 4;
  return 0;
}
