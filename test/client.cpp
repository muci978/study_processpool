#include <sys/socket.h>
#include <stdio.h>
#include <arpa/inet.h>
#include <string.h>
#include <stdlib.h>
#include <unistd.h>

int main(int argc, char** argv){
    int conn = socket(PF_INET, SOCK_STREAM, 0);
    sockaddr_in addr;
    bzero(&addr, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port = htons(atoi(argv[2]));
    inet_pton(AF_INET, argv[1], &addr.sin_addr);
    connect(conn, (sockaddr*)&addr, sizeof(addr));

    char buf[1024];
    memset(buf, '\0', 1024);
    send(conn, "helloworld\r\n", 10, 0);
    int i = recv(conn, buf, 10, 0);
    printf("%s\n", buf);
    close(conn);
}