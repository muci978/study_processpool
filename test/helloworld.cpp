#include <stdio.h>
#include <unistd.h>

int main(){
    write(STDOUT_FILENO, "helloworld\n", 13);
}