// [over.ics.list], [over.ics.rank], [dcl.init.list] (#231)
struct Integral {
    int first;
    int second;
};

struct Fractional {
    double first;
    int second;
};

int choose(Integral, int);
int choose(Fractional, long);

int main() {
    return choose({1.25, 2}, {0}); //! error
}
