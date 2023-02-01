#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <stdio.h>
#include <unistd.h>
#include <errno.h>
#include <fcntl.h>
#include <stdlib.h>
#include <cassert>

#include "./lock/locker.h"
#include "./threadpool/threadpool.h"
#include "./timer/lst_timer.h"
#include "./http/http_conn.h"
#include "./log/log.h"
#include "./CGImysql/sql_connection_pool.h"
#include "./io_uring/io_uring.h"
#include "./queue/queue.h"

#define MAX_FD 65536           // 最大文件描述符
#define MAX_EVENT_NUMBER 10000 // 最大事件数
#define TIMESLOT 5             // 最小超时单位

#define SYNLOG // 同步写日志
// #define ASYNLOG //异步写日志

// 在http_conn.cpp中定义，改变链接属性
extern int setnonblocking(int fd);

extern Queue<int> write_queue;

int ringque = 0;

// 设置定时器相关参数
static int pipefd[2];
static sort_timer_lst timer_lst;

struct io_uring ring;

// 信号处理函数
void sig_handler(int sig)
{
    // 为保证函数的可重入性，保留原来的errno
    int save_errno = errno;
    int msg = sig;
    send(pipefd[1], (char *)&msg, 1, 0);
    errno = save_errno;
}

// 设置信号函数
void addsig(int sig, void(handler)(int), bool restart = true)
{
    struct sigaction sa;
    memset(&sa, '\0', sizeof(sa));
    sa.sa_handler = handler;
    if (restart)
        sa.sa_flags |= SA_RESTART;
    sigfillset(&sa.sa_mask);
    assert(sigaction(sig, &sa, NULL) != -1);
}

// 定时处理任务，重新定时以不断触发SIGALRM信号
void timer_handler()
{
    timer_lst.tick();
    alarm(TIMESLOT);
}

// 定时器回调函数，删除非活动连接在socket上的注册事件，并关闭
void cb_func(client_data *user_data)
{
    // epoll_ctl(epollfd, EPOLL_CTL_DEL, user_data->sockfd, 0);
    assert(user_data);
    // printf("定时器删除fd%d\n", user_data->sockfd);
    close(user_data->sockfd);
    http_conn::m_user_count--;
    LOG_INFO("close fd %d", user_data->sockfd);
}

void show_error(int connfd, const char *info)
{
    printf("%s", info);
    send(connfd, info, strlen(info), 0);
    close(connfd);
}

int add_accept_request(int server_socket, struct sockaddr_in *client_addr,
                       socklen_t *client_addr_len)
{
    struct io_uring_sqe *sqe = io_uring_get_sqe(&ring);
    io_uring_prep_accept(sqe, server_socket, (struct sockaddr *)client_addr,
                         client_addr_len, 0);
    struct request *req = (struct request *)malloc(sizeof(*req));
    req->event_type = EVENT_TYPE_ACCEPT;
    // printf("add_accept_request,myid = %d", __myid);
    // req->client_socket = __myid;
    io_uring_sqe_set_data(sqe, req);
    io_uring_submit(&ring);
    ringque++;
    return 0;
}

int main(int argc, char *argv[])
{
#ifdef ASYNLOG
    Log::get_instance()->init("ServerLog", 2000, 800000, 8); // 异步日志模型
#endif

#ifdef SYNLOG
    Log::get_instance()->init("ServerLog", 2000, 800000, 0); // 同步日志模型
#endif

    if (argc <= 1)
    {
        printf("usage: %s ip_address port_number\n", basename(argv[0]));
        return 1;
    }

    int port = atoi(argv[1]);

    addsig(SIGPIPE, SIG_IGN);

    // 创建数据库连接池
    connection_pool *connPool = connection_pool::GetInstance();
    connPool->init("localhost", "root", "010422", "serverdb", 3306, 8);

    // 创建线程池
    threadpool<http_conn> *pool = NULL;
    try
    {
        pool = new threadpool<http_conn>(connPool);
    }
    catch (...)
    {
        return 1;
    }

    http_conn *users = new http_conn[MAX_FD];
    assert(users);

    // 初始化数据库读取表
    users->initmysql_result(connPool);

    int listenfd = socket(PF_INET, SOCK_STREAM, 0);
    assert(listenfd >= 0);

    // struct linger tmp={1,0};
    // SO_LINGER若有数据待发送，延迟关闭
    // setsockopt(listenfd,SOL_SOCKET,SO_LINGER,&tmp,sizeof(tmp));

    int ret = 0;
    struct sockaddr_in address;
    bzero(&address, sizeof(address));
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = htonl(INADDR_ANY);
    address.sin_port = htons(port);

    int flag = 1;
    setsockopt(listenfd, SOL_SOCKET, SO_REUSEADDR, &flag, sizeof(flag));
    ret = bind(listenfd, (struct sockaddr *)&address, sizeof(address));
    assert(ret >= 0);
    ret = listen(listenfd, 10);
    assert(ret >= 0);

    // 初始化ring
    io_uring_queue_init(QUEUE_DEPTH, &ring, 0);

    // 创建管道
    // ret = socketpair(PF_UNIX, SOCK_STREAM, 0, pipefd);
    // assert(ret != -1);
    // setnonblocking(pipefd[1]);
    // addfd(epollfd, pipefd[0], false);

    // addsig(SIGALRM, sig_handler, false);
    // addsig(SIGTERM, sig_handler, false);

    bool stop_server = false;

    // client_data *users_timer = new client_data[MAX_FD];

    bool timeout = false;
    // alarm(TIMESLOT);

    // init io_uring
    struct io_uring_cqe *cqe;
    struct sockaddr_in client_addr;
    socklen_t client_addr_len = sizeof(client_addr);
    add_accept_request(listenfd, &client_addr, &client_addr_len);

    // main loop
    while (!stop_server)
    {
        while (write_queue.empty() != true)
        {
            int fd = write_queue.pop();
            // printf("开始处理fd=%d的写事件\n", fd);
            users[fd].write();
            // printf("处理完成...\n");
        }

        // int ret = io_uring_submit_and_wait(&ring, 1);
        // printf("wait cqe....\n");
        int ret = io_uring_peek_cqe(&ring, &cqe);
        if (ret < 0)
        {
            continue;
            // perror("io_uring_peek_cqe");
            // exit(1);
        }
        // if (cqe == NULL)
        //{
        // continue;
        //}
        // unsigned head;
        unsigned count = 0;

        // io_uring_for_each_cqe(&ring, head, cqe)
        //{
        ++count;
        struct request *req = (struct request *)cqe->user_data;

        if (cqe->res < 0)
        {
            if (req->event_type == EVENT_TYPE_ACCEPT)
            {
                // printf("接受新的客户端！,接受到的id为 %d ###########################\n", myid);
                add_accept_request(listenfd, &client_addr, &client_addr_len);
            }
            fprintf(stderr, "Async request failed: %s for event: %d,client socket is %d\n",
                    strerror(-cqe->res), req->event_type, req->client_socket);
            // continue;
            exit(1);
        }

        switch (req->event_type)
        {
        case EVENT_TYPE_ACCEPT:
            // printf("####################接受新的客户端！###########################\n");
            add_accept_request(listenfd, &client_addr, &client_addr_len);
            users[cqe->res].init(cqe->res, client_addr);
            // printf("第%d次accept\n", ++accept);
            // add_read_request(cqe->res);
            users[cqe->res].read_once();
            // printf("从fd%d读数据\n", cqe->res);
            free(req);
            break;
        case EVENT_TYPE_READ:
            if (cqe->res == 0)
            {
                fprintf(stderr, "Empty request!\n");
                close(req->client_socket);
                break;
            }
            users[req->client_socket].m_read_idx += cqe->res;
            // printf("读到%d字节数据\n", cqe->res);
            pool->append(users + req->client_socket);
            // io_uring_submit(&ring);
            // add_read_request(req->client_socket);
            // free(req->iov[0].iov_base);
            // users[req->client_socket].write();
            break;
        case EVENT_TYPE_WRITE:
            // add_accept_request(listenfd, &client_addr, &client_addr_len, myid++);
            // printf("需要传输的字节数：%d\n", users[req->client_socket].bytes_to_send);
            // printf("传输的字节数：%d\n", cqe->res);
            if (cqe->res < users[req->client_socket].bytes_to_send)
            {
                users[req->client_socket].bytes_to_send -= cqe->res;
                users[req->client_socket].bytes_have_send += cqe->res;
                users[req->client_socket].write();
                // io_uring_submit(&ring);
                break;
            }
            // for (int i = 0; i < req->iovec_count; i++)
            // {
            //     free(req->iov[i].iov_base);
            // }
            // printf("监听fd%d\n", req->client_socket);
            if (users[req->client_socket].m_linger == true)
            {
                users[req->client_socket].unmap();
                // users[req->client_socket].init();
                // users[req->client_socket].read_once();
                // printf("keep alive\n");
                http_conn::m_user_count--;
                close(req->client_socket);
                //  return true;
            }
            else
            {
                users[req->client_socket].unmap();
                // printf("not keep alive\n");
                http_conn::m_user_count--;
                close(req->client_socket);
                // return false;
            }
            // add_read_request(req->client_socket);
            free(req);
            break;
        }
        /* Mark this request as processed */
        io_uring_cqe_seen(&ring, cqe);
        //}
        // io_uring_cq_advance(&ring, count);
        ringque -= count;
        // printf("目前io_uring队列数量： %d\n", ringque);
        if (ringque > 8100)
        {
            // printf("ringque > 8100\n");
            //  exit(1);
        }
        //         int number = epoll_wait(epollfd, events, MAX_EVENT_NUMBER, -1);
        //         if (number < 0 && errno != EINTR)
        //         {
        //             LOG_ERROR("%s", "epoll failure");
        //             break;
        //         }

        //         for (int i = 0; i < number; i++)
        //         {
        //             int sockfd = events[i].data.fd;

        //             // 处理新到的客户连接
        //             if (sockfd == listenfd)
        //             {
        //                 struct sockaddr_in client_address;
        //                 socklen_t client_addrlength = sizeof(client_address);
        // #ifdef listenfdLT
        //                 int connfd = accept(listenfd, (struct sockaddr *)&client_address, &client_addrlength);
        //                 if (connfd < 0)
        //                 {
        //                     LOG_ERROR("%s:errno is:%d", "accept error", errno);
        //                     continue;
        //                 }
        //                 if (http_conn::m_user_count >= MAX_FD)
        //                 {
        //                     show_error(connfd, "Internal server busy");
        //                     LOG_ERROR("%s", "Internal server busy");
        //                     continue;
        //                 }
        //                 users[connfd].init(connfd, client_address);

        //                 // 初始化client_data数据
        //                 // 创建定时器，设置回调函数和超时时间，绑定用户数据，将定时器添加到链表中
        //                 users_timer[connfd].address = client_address;
        //                 users_timer[connfd].sockfd = connfd;
        //                 util_timer *timer = new util_timer;
        //                 timer->user_data = &users_timer[connfd];
        //                 timer->cb_func = cb_func;
        //                 time_t cur = time(NULL);
        //                 timer->expire = cur + 3 * TIMESLOT;
        //                 users_timer[connfd].timer = timer;
        //                 timer_lst.add_timer(timer);
        // #endif

        // #ifdef listenfdET
        //                 while (1)
        //                 {
        //                     int connfd = accept(listenfd, (struct sockaddr *)&client_address, &client_addrlength);
        //                     if (connfd < 0)
        //                     {
        //                         LOG_ERROR("%s:errno is:%d", "accept error", errno);
        //                         break;
        //                     }
        //                     if (http_conn::m_user_count >= MAX_FD)
        //                     {
        //                         show_error(connfd, "Internal server busy");
        //                         LOG_ERROR("%s", "Internal server busy");
        //                         break;
        //                     }
        //                     users[connfd].init(connfd, client_address);

        //                     // 初始化client_data数据
        //                     // 创建定时器，设置回调函数和超时时间，绑定用户数据，将定时器添加到链表中
        //                     users_timer[connfd].address = client_address;
        //                     users_timer[connfd].sockfd = connfd;
        //                     util_timer *timer = new util_timer;
        //                     timer->user_data = &users_timer[connfd];
        //                     timer->cb_func = cb_func;
        //                     time_t cur = time(NULL);
        //                     timer->expire = cur + 3 * TIMESLOT;
        //                     users_timer[connfd].timer = timer;
        //                     timer_lst.add_timer(timer);
        //                 }
        //                 continue;
        // #endif
        //             }

        //             else if (events[i].events & (EPOLLRDHUP | EPOLLHUP | EPOLLERR))
        //             {
        //                 // 服务器端关闭连接，移除对应的定时器
        //                 printf("服务器端关闭连接，移除对应的定时器\n");
        //                 util_timer *timer = users_timer[sockfd].timer;
        //                 timer->cb_func(&users_timer[sockfd]);

        //                 if (timer)
        //                 {
        //                     timer_lst.del_timer(timer);
        //                 }
        //             }

        //             // 处理信号
        //             else if ((sockfd == pipefd[0]) && (events[i].events & EPOLLIN))
        //             {
        //                 int sig;
        //                 char signals[1024];
        //                 ret = recv(pipefd[0], signals, sizeof(signals), 0);
        //                 if (ret == -1)
        //                 {
        //                     continue;
        //                 }
        //                 else if (ret == 0)
        //                 {
        //                     continue;
        //                 }
        //                 else
        //                 {
        //                     for (int i = 0; i < ret; ++i)
        //                     {
        //                         switch (signals[i])
        //                         {
        //                         case SIGALRM:
        //                         {
        //                             timeout = true;
        //                             break;
        //                         }
        //                         case SIGTERM:
        //                         {
        //                             stop_server = true;
        //                         }
        //                         }
        //                     }
        //                 }
        //             }

        //             // 处理客户连接上接收到的数据
        //             else if (events[i].events & EPOLLIN)
        //             {
        //                 util_timer *timer = users_timer[sockfd].timer;
        //                 int myret;
        //                 if (myret = users[sockfd].read_once())
        //                 {
        //                     LOG_INFO("deal with the client(%s)", inet_ntoa(users[sockfd].get_address()->sin_addr));
        //                     // 若监测到读事件，将该事件放入请求队列
        //                     pool->append(users + sockfd);

        //                     // 若有数据传输，则将定时器往后延迟3个单位
        //                     // 并对新的定时器在链表上的位置进行调整
        //                     if (timer)
        //                     {
        //                         time_t cur = time(NULL);
        //                         timer->expire = cur + 3 * TIMESLOT;
        //                         LOG_INFO("%s", "adjust timer once");
        //                         timer_lst.adjust_timer(timer);
        //                     }
        //                 }
        //                 else
        //                 {
        //                     printf("my read ret == %d\n", myret);
        //                     timer->cb_func(&users_timer[sockfd]);
        //                     if (timer)
        //                     {
        //                         timer_lst.del_timer(timer);
        //                     }
        //                 }
        //             }
        //             else if (events[i].events & EPOLLOUT)
        //             {
        //                 util_timer *timer = users_timer[sockfd].timer;
        //                 int myret;
        //                 printf("EPOLLOUT\n");
        //                 if (myret = users[sockfd].write())
        //                 {
        //                     LOG_INFO("send data to the client(%s)", inet_ntoa(users[sockfd].get_address()->sin_addr));

        //                     // 若有数据传输，则将定时器往后延迟3个单位
        //                     // 并对新的定时器在链表上的位置进行调整
        //                     if (timer)
        //                     {
        //                         time_t cur = time(NULL);
        //                         timer->expire = cur + 3 * TIMESLOT;
        //                         LOG_INFO("%s", "adjust timer once");
        //                         timer_lst.adjust_timer(timer);
        //                     }
        //                 }
        //                 else
        //                 {
        //                     printf("my write ret == %d\n", myret);
        //                     timer->cb_func(&users_timer[sockfd]);
        //                     if (timer)
        //                     {
        //                         timer_lst.del_timer(timer);
        //                     }
        //                 }
        //             }
        //         }
        //         if (timeout)
        //         {
        //             timer_handler();
        //             timeout = false;
        //         }
    }
    close(listenfd);
    // close(pipefd[1]);
    // close(pipefd[0]);
    delete[] users;
    // delete[] users_timer;
    delete pool;
    return 0;
}
