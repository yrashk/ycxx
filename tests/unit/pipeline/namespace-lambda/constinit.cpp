static int bias=0; static constinit auto triple=[](int x){return x*3+bias;}; static auto other=[](int x){return x*3;}; int main(){bias=3; return (void*)&other==(void*)&triple || triple(13)!=42;}
