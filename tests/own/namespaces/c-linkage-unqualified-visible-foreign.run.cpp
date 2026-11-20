// Correspondence requires the same target scope ([basic.link], #455).
namespace COnly {
extern "C" int visible(int x) { return x + 2; }
}
namespace ViaUsing {
using namespace COnly;
int visible(int x) { return x + 3; }
}
namespace Outer {
extern "C" int outer(int x) { return x + 4; }
namespace Inner {
int outer(int x) { return x + 5; }
}
}
int main() {
  return COnly::visible(40) == 42 && ViaUsing::visible(40) == 43 &&
         Outer::outer(40) == 44 && Outer::Inner::outer(40) == 45 ? 0 : 1;
}
