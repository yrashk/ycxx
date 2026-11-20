// [over.ics.list], [over.ics.rank], [dcl.init.list] (#231)
int choose(unsigned int, int);
int choose(long, long);

int main() {
    return choose({-1}, {0}); //! error
}
