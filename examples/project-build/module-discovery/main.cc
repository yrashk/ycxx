import demo.score;

#if defined(__linux__)
import demo.linux;
#elif defined(__APPLE__)
import demo.darwin;
#endif

/*
import demo.comment;
*/

static_assert(sizeof(R"source(
import demo.raw_string;
)source") > 1);

int main() {
  return 0;
}
