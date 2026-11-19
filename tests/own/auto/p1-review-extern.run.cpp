// θ step 1 (#279), from the review: `extern auto x = e;` is a definition ([basic.def]/2).
extern auto a = 1;
extern const auto b = 2;
int main() { return a + b == 3 ? 0 : 1; }
