// [over.ics.list], [over.ics.rank], [dcl.init.list] (#231)
int cross(int, long);
int cross(long, int);

int main() {
    return cross({1}, {2}); //! error
}
