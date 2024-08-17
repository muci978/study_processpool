#include <sys/types.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <assert.h>
#include <stdio.h>
#include <unistd.h>
#include <errno.h>
#include <string.h>
#include <fcntl.h>
#include <stdlib.h>
#include <sys/epoll.h>
#include <signal.h>
#include <sys/wait.h>
#include <sys/stat.h>

#include "processpool.h"

/**
 * 当 Web 服务器收到一个对 CGI 程序的请求时，会启动一个新的进程来执行这个程序。
 * 这个程序可以是任何可以生成动态内容的脚本或可执行文件。
 * 程序执行完成后，将结果返回给 Web 服务器，服务器再将结果返回给客户端。
 */

class cgi_conn
{
public:
    cgi_conn() {}
    ~cgi_conn() {}

    // 初始化客户端连接，清空缓存区
    void init(int epollfd, int sockfd, const sockaddr_in &client_addr)
    {
        m_epollfd = epollfd;
        m_sockfd = sockfd;
        m_address = client_addr;
        memset(m_buf, '\0', BUFFER_SIZE);
        m_read_idx = 0;
    }

    void process()
    {
        int idx = 0;
        int ret = -1;
        // 循环读取和分析客户数据
        while (true)
        {
            idx = m_read_idx;
            ret = recv(m_sockfd, m_buf + idx, BUFFER_SIZE - 1 - idx, 0);
            if (ret < 0)
            {
                if (errno != EAGAIN)
                {
                    removefd(m_epollfd, m_sockfd);
                    break;
                }
                else if (ret == 0)
                {
                    removefd(m_epollfd, m_sockfd);
                    break;
                }
                else
                {
                    m_read_idx += ret;
                    printf("user content is: %s\n", m_buf);
                    // 如果遇到字符“\r\n”则开始处理客户请求
                    for (; idx < m_read_idx; ++idx)
                    {
                        if ((idx >= 1) && (m_buf[idx - 1] == '\r' && m_buf[idx] == '\n'))
                        {
                            break;
                        }
                    }
                    // 如果没有遇到字符“\r\n”，则需要读取更多的字符
                    if (idx == m_read_idx)
                    {
                        continue;
                    }
                    m_buf[idx - 1] = '\0';

                    char *file_name = m_buf;
                    // 判断客户要执行的cgi程序是否存在
                    if (access(file_name, F_OK) == -1)
                    {
                        removefd(m_epollfd, m_sockfd);
                        break;
                    }

                    // 创建子进程来执行cgi程序
                    ret = fork();
                    if (ret == -1)
                    {
                        removefd(m_epollfd, m_sockfd);
                        break;
                    }
                    else if (ret > 0)
                    {
                        // 父进程只需要关闭连接
                        removefd(m_epollfd, m_sockfd);
                        break;
                    }
                    else
                    {
                        // 子进程将标准输出定向到m_sockfd，并执行cgi程序
                        printf("task start\n");
                        close(STDOUT_FILENO);
                        dup(m_sockfd);
                        execl(m_buf, m_buf, nullptr);
                        exit(0);
                    }
                }
            }
        }
    }

private:
    // 读缓冲区的大小
    static const int BUFFER_SIZE = 1024;
    // 每个子进程只有一个epoll实例，所以为静态
    static int m_epollfd;
    int m_sockfd;
    sockaddr_in m_address;
    char m_buf[BUFFER_SIZE];
    // 标记读缓冲中已经读入的客户数据中的最后一个字节的下一个位置
    int m_read_idx;
};

int cgi_conn::m_epollfd = -1;

int main(int argc, char **argv)
{
    if (argc <= 2)
    {
        printf("usage: %s ip_address port_number\n", basename(argv[0]));
        return 1;
    }

    const char *ip = argv[1];
    int port = atoi(argv[2]);

    int listenfd = socket(PF_INET, SOCK_STREAM, 0);
    assert(listenfd > 0);

    int ret = 0;
    sockaddr_in addr;
    bzero(&addr, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port = htons(port);
    inet_pton(AF_INET, ip, &addr.sin_addr);

    ret = bind(listenfd, (sockaddr *)&addr, sizeof(addr));
    assert(ret != -1);
    ret = listen(listenfd, 5);
    assert(ret != -1);

    processpool<cgi_conn> *pool = processpool<cgi_conn>::create(listenfd);
    if (pool)
    {
        pool->run();
        delete pool;
    }
    close(listenfd);
    return 0;
}