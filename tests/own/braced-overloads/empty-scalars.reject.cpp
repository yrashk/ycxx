// [over.ics.list], [over.ics.rank], [dcl.init.list] (#231)
int choose(int);
int choose(long);

int main() {
    return choose({}); //! error
}
