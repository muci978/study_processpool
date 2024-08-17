#ifndef PROCESSPOOL_H
#define PROCESSPOOL_H

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

/*
这是改进版本的半同步半反应堆模型的进程池。
主进程只负责管理监听socket，每个子进程各自维护一个epoll，每个子进程可以管理多个事件。
为了避免在父子进程间传递文件描述符，当建立新的连接时，由父进程通知子进程，子进程连接，为此子进程必须拷贝父进程的监听socket。
*/

// 描述一个子进程
class process
{
public:
    process() : m_pid(-1) {}
    pid_t m_pid;
    int m_pipefd[2];
};

// 进程池类，为了代码复用定义成模板类，模板参数时处理逻辑任务的类
// 这只是一个管理多进程的部件，服务器的具体业务逻辑还需要依靠T来传入
template <typename T>
class processpool
{
private:
    // 构造函数定义成私有的，我们只能用静态函数create来创建实例
    processpool(int listenfd, int process_number = 8);

public:
    // 单例模式，保证程序最多创建一个进程池实例，保证程序能正确处理信号的必要条件
    static processpool<T> *create(int listenfd, int process_number = 8)
    {
        if (!m_instance)
        {
            m_instance = new processpool<T>(listenfd, process_number);
        }
        return m_instance;
    }

    ~processpool()
    {
        delete[] m_sub_process;
    }

    // 启动进程池
    void run();

private:
    void setup_sig_pipe();
    void run_parent();
    void run_child();

private:
    // 最大子进程数目
    static const int MAX_PROCESS_NUMBER = 16;
    // 每个子进程能处理的最大用户数
    static const int USER_PER_PROCESS = 65536;
    // epoll能处理的最大事件数
    static const int MAX_EVENT_NUMBER = 10000;
    // 子进程数目
    int m_process_number;
    // 子进程在进程池里的编号
    int m_idx;
    // 每个进程都有一个epoll内核事件表，用m_epollfd标识
    int m_epollfd;
    // 监听socket
    int m_listenfd;
    // 子进程的结束标志
    int m_stop;
    // 描述子进程信息的数组
    process *m_sub_process;
    // 进程池的唯一实例
    static processpool<T> *m_instance;
};
template <typename T>
processpool<T> *processpool<T>::m_instance = nullptr;

// 进程池构造函数，参数listenfd是监听socket，它必须在创建线程池实例前被创建，否则子进程无法直接引用它
template <typename T>
processpool<T>::processpool(int listenfd, int process_number)
    : m_listenfd(listenfd), m_process_number(process_number), m_idx(-1), m_stop(false)
{
    assert((process_number > 0) && (process_number <= MAX_PROCESS_NUMBER));

    m_sub_process = new process[process_number];
    assert(m_sub_process);

    // 创建process_number个子线程，并建立它们与父进程之间的管道
    for (int i = 0; i < process_number; ++i)
    {
        int ret = socketpair(AF_UNIX, SOCK_STREAM, 0, m_sub_process[i].m_pipefd);
        assert(ret == 0);
        m_sub_process[i].m_pid = fork();
        assert(m_sub_process[i].m_pid >= 0);
        if (m_sub_process[i].m_pid > 0)
        {
            close(m_sub_process[i].m_pipefd[1]);
            continue;
        }
        else
        {
            close(m_sub_process[i].m_pipefd[0]);
            // 子进程会拷贝父进程的地址空间，包括静态数据，子进程通过拷贝过来的进程池实例来判断自己在父进程中的进程池的序号
            m_idx = i;
            break;
        }
    }
}

// 设置文件描述符为非阻塞
static int setnonblocking(int fd)
{
    int old_option = fcntl(fd, F_GETFL);
    fcntl(fd, F_SETFL, old_option | O_NONBLOCK);
    return old_option;
}

// 将文件描述符加入epoll的内核事件表
static void addfd(int epollfd, int fd)
{
    epoll_event event;
    event.events = EPOLLET | EPOLLIN;
    event.data.fd = fd;
    epoll_ctl(epollfd, EPOLL_CTL_ADD, fd, &event);
    setnonblocking(fd);
}

// 从epoll的内核事件表上删除文件描述符的所有注册事件，并关闭文件描述符
static void removefd(int epollfd, int fd)
{
    epoll_ctl(epollfd, EPOLL_CTL_DEL, fd, nullptr);
    close(fd);
}

// 用于统一事件源的信号管道
static int sig_pipefd[2];

// 信号处理函数，实际是将信号转发给epoll统一处理
void sig_handler(int sig)
{
    // 可重入函数，需要保存并恢复状态
    int save_errno = errno;
    int msg = sig;
    send(sig_pipefd[1], &msg, 1, 0);
    errno = save_errno;
}

// 绑定信号和信号处理函数，实际上是将信号转发给epoll统一处理
static void addsig(int sig, void (*handler)(int), bool restart = true)
{
    struct sigaction act;
    bzero(&act, sizeof(act));
    act.sa_handler = handler;
    if (restart)
    {
        act.sa_flags |= SA_RESTART;
    }
    sigfillset(&act.sa_mask);
    assert(sigaction(sig, &act, nullptr) != -1);
}

// 设置统一事件源
// 每个子进程拥有各自的统一事件源
template <typename T>
void processpool<T>::setup_sig_pipe()
{
    // m_epollfd是类的成员，子进程的epoll例程是在这里申请的，也就是说每个子进程都要执行该函数
    m_epollfd = epoll_create(5);
    assert(m_epollfd != -1);

    int ret = socketpair(AF_UNIX, SOCK_STREAM, 0, sig_pipefd);
    assert(ret != -1);

    // sig_pipefd[1]是全双工管道的信号段，将其设置为非阻塞是为了减少信号处理函数的执行时间，防止因为阻塞而丢失信号
    setnonblocking(sig_pipefd[1]);
    addfd(m_epollfd, sig_pipefd[0]);

    addsig(SIGCHLD, sig_handler);
    addsig(SIGTERM, sig_handler);
    addsig(SIGINT, sig_handler);
    addsig(SIGPIPE, SIG_IGN);
}

// 父进程中m_idx为-1，子进程中m_idx在进程池初始化时被赋值并拷贝，根据此判断是父进程还是子进程
template <typename T>
void processpool<T>::run()
{
    if (m_idx != -1)
    {
        run_child();
        return;
    }
    run_parent();
}

template <typename T>
void processpool<T>::run_child()
{
    // 设置统一事件源，并取得epoll实例
    setup_sig_pipe();

    // 将与父进程之间的管道加入监听
    int pipefd = m_sub_process[m_idx].m_pipefd[1];
    addfd(m_epollfd, pipefd);

    epoll_event events[MAX_EVENT_NUMBER];
    T *users = new T[USER_PER_PROCESS];
    assert(users);
    int number = 0;
    int ret = -1;

    while (!m_stop)
    {
        number = epoll_wait(m_epollfd, events, MAX_EVENT_NUMBER, -1);
        if (number < 0 && errno != EINTR)
        {
            printf("epoll failure\n");
            break;
        }

        for (int i = 0; i < number; ++i)
        {
            int sockfd = events[i].data.fd;
            if (sockfd == pipefd && events[i].events & EPOLLIN)
            {
                int client = 0;
                ret = recv(sockfd, &client, sizeof(client), 0);
                // 如果发生错误或者关闭管道则什么都不做
                // 成功或者还数据未到达则进行accept，因为client只是主进程对子进程的通知，这代表有连接到达，并不具有实际数据
                // 主进程和子进程拥有相同的监听socket，主进程监听这个套接字而子进程不会，当有连接到达时主进程通知子进程连接
                // syn队列和accept队列都是内核管理，属于套接字的，所以子进程可以直接从accept队列拿取连接
                if ((ret == -1 && errno != EAGAIN) || ret == 0)
                {
                    continue;
                }
                else
                {
                    sockaddr_in addr;
                    socklen_t client_addrlength = sizeof(addr);
                    int connfd = accept(m_listenfd, (sockaddr *)&addr, &client_addrlength);
                    if (connfd < 0)
                    {
                        printf("errno is: %d\n", errno);
                        continue;
                    }
                    addfd(m_epollfd, connfd);
                    // 模板参数T必须实现init方法，以初始化一个客户端连接
                    // 我们直接使用connfd来索引逻辑处理对象，以提高程序效率
                    users[connfd].init(m_epollfd, connfd, addr);
                }
            }
            else if (sockfd == sig_pipefd[0] && events[i].events & EPOLLIN)
            {
                char signals[1024];
                ret = recv(sockfd, signals, 1024, 0);
                if (ret <= 0)
                {
                    continue;
                }
                else
                {
                    for (int i = 0; i < ret; ++i)
                    {
                        switch (signals[i])
                        {
                        case SIGCHLD:
                        {
                            pid_t pid;
                            int stat = 0;
                            // 可能有多个子进程同时通知，但是只会接收并处理一个信号
                            while ((pid = waitpid(-1, &stat, WNOHANG)) > 0)
                            {
                                continue;
                            }
                            break;
                        }
                        case SIGTERM:
                        case SIGINT:
                        {
                            m_stop = true;
                            break;
                        }
                        default:
                        {
                            break;
                        }
                        }
                    }
                }
            }
            // 如果有其他可读数据，那么一定是客户请求到来，调用逻辑处理对象的process进行处理
            else if (events[i].events & EPOLLIN)
            {
                users[sockfd].process();
            }
            else
            {
                continue;
            }
        }
    }
    delete[] users;
    users = nullptr;
    close(pipefd);
    close(m_epollfd);
    // close(m_listenfd); 这个套接字应该由它的创建者销毁。对象（如文件描述符），由哪个函数创建，就由哪个函数销毁。
}

template <typename T>
void processpool<T>::run_parent()
{
    // 设置了统一事件源
    // 创建epoll实例
    setup_sig_pipe();

    addfd(m_epollfd, m_listenfd);

    epoll_event events[MAX_EVENT_NUMBER];
    int sub_process_counter = 0;
    int new_conn = 1;
    int number = 0;
    int ret = -1;

    while (!m_stop)
    {
        number = epoll_wait(m_epollfd, events, MAX_EVENT_NUMBER, -1);
        if (number < 0 && errno != EINTR)
        {
            printf("epoll failure\n");
            break;
        }
        for (int i = 0; i < number; ++i)
        {
            int sockfd = events[i].data.fd;
            if (sockfd == m_listenfd)
            {
                // 如果有新的连接到来，就采用round robin方式将其分配给一个子进程处理
                int i = sub_process_counter;
                do
                {
                    if (m_sub_process[i].m_pid != -1)
                    {
                        break;
                    }
                    i = (i + 1) % m_process_number;
                } while (i != sub_process_counter);

                if (m_sub_process[i].m_pid == -1)
                {
                    m_stop = true;
                    break;
                }
                sub_process_counter = (i + 1) % m_process_number;
                send(m_sub_process[i].m_pipefd[0], &new_conn, sizeof(new_conn), 0);
                printf("send request to child %d\n", i);
            }

            else if (sockfd == sig_pipefd[0] && events[i].events & EPOLLIN)
            {
                char signals[1024];
                ret = recv(sig_pipefd[0], signals, sizeof(signals), 0);
                if (ret <= 0)
                {
                    continue;
                }
                else
                {
                    for (int i = 0; i < ret; ++i)
                    {
                        switch (signals[i])
                        {
                        case SIGCHLD:
                        {
                            pid_t pid;
                            int stat;
                            while ((pid = waitpid(-1, &stat, WNOHANG)) > 0)
                            {
                                for (int i = 0; i < m_process_number; ++i)
                                {
                                    // 若进程池中第i个子进程退出了，则主进程关闭对应的通信管道，并设置相应的m_pid=-1
                                    if (m_sub_process[i].m_pid == pid)
                                    {
                                        printf("child %d join\n", i);
                                        close(m_sub_process[i].m_pipefd[0]);
                                        m_sub_process[i].m_pid = -1;
                                    }
                                }
                            }
                            // 如果所有子进程都退出了，父进程也退出
                            m_stop = true;
                            for (int i = 0; i < m_process_number; ++i)
                            {
                                if (m_sub_process[i].m_pid != -1)
                                {
                                    m_stop = false;
                                }
                            }
                            break;
                        }
                        case SIGTERM:
                        case SIGINT:
                        {
                            printf("kill all the child now\n");
                            for (int i = 0; i < m_process_number; ++i)
                            {
                                int pid = m_sub_process[i].m_pid;
                                if (pid != -1)
                                {
                                    kill(pid, SIGTERM);
                                }
                            }
                            break;
                        }
                        default:
                        {
                            break;
                        }
                        }
                    }
                }
            }
            else
            {
                break;
            }
        }
    }
    // close(m_listenfd);
    close(m_epollfd);
}

#endif