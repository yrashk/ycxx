static int bias=0; static auto triple=[](int x){return x*3+bias;}; int main(){bias=3; return triple(13)-42;}
