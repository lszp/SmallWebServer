#include "webserver.h"

WebServer::WebServer()
{
    //http_conn类对象
    users = new http_conn[MAX_FD];

    //root文件夹路径
    char server_path[200];
    getcwd(server_path, 200);
    char root[6] = "/root";
    m_root = (char *)malloc(strlen(server_path) + strlen(root) + 1);
    strcpy(m_root, server_path);
    strcat(m_root, root);

    //定时器
    users_timer = new client_data[MAX_FD];
}

WebServer::~WebServer()
{
    close(m_epollfd);
    close(m_listenfd);
    close(m_pipefd[1]);
    close(m_pipefd[0]);
    delete[] users;
    delete[] users_timer;
    delete m_pool;
}

void WebServer::init(int port, string user, string passWord, string databaseName, int log_write, int sqlverify,
                     int opt_linger, int trigmode, int sql_num, int thread_num, int close_log, int actor_model)
{
    m_port = port;
    m_user = user;
    m_passWord = passWord;
    m_databaseName = databaseName;
    m_sql_num = sql_num;
    m_thread_num = thread_num;
    m_log_write = log_write;
    m_SQLVerify = sqlverify;
    m_OPT_LINGER = opt_linger;
    m_TRIGMode = trigmode;
    m_close_log = close_log;
    m_actormodel = actor_model;
}

void WebServer::log_write()
{
    if (0 == m_close_log)
    {
        //初始化日志
        if (1 == m_log_write)
            Log::get_instance()->init("./ServerLog", m_close_log, 2000, 800000, 800);
        else
            Log::get_instance()->init("./ServerLog", m_close_log, 2000, 800000, 0);
    }
}

void WebServer::sql_pool()
{
    //初始化数据库连接池
    m_connPool = connection_pool::GetInstance();
    m_connPool->init("localhost", m_user, m_passWord, m_databaseName, 3306, m_sql_num, m_close_log);

    //初始化数据库读取表
    if (0 == m_SQLVerify)
        users->initmysql_result(m_connPool);
    else if (1 == m_SQLVerify)
        users->initresultFile(m_connPool);
}

void WebServer::thread_pool()
{
    //线程池
    m_pool = new threadpool<http_conn>(m_actormodel, m_connPool, m_thread_num);
}

void WebServer::eventListen()
{
    //网络编程基础步骤
    m_listenfd = socket(PF_INET, SOCK_STREAM, 0); // IPv4, TCP
    assert(m_listenfd >= 0);

    //优雅关闭连接
    if (0 == m_OPT_LINGER)
    {
        struct linger tmp = {0, 1};  // struct linger{int l_onoff, int l_linger}，l_onoff=0, l_linger忽略，调用closesocker断开TCP连接时，低层将未发送完的数据发送完再释放资源。
        setsockopt(m_listenfd, SOL_SOCKET, SO_LINGER, &tmp, sizeof(tmp));  //处于连接状态的socket调用closesocker后强制关闭，不经历TIME_WAIT过程
    }
    else if (1 == m_OPT_LINGER)
    {
        struct linger tmp = {1, 1}; // l_onoff非零，l_linger>0，调用closesocker不会立即返回，内核延迟一段时间，时间由l_linger决定，
                                    // 如果超时时间到达之前，发送完未发送的数据（包括FIN包）并得到另一端的确认，closesocket返回正确，socket描述符优雅退出，否则closesocket返回错误，
                                    // 未发送数据流失，socket描述符被强制退出。注意，如果socket描述符设置为非堵塞型，closesocket直接返回值。
        setsockopt(m_listenfd, SOL_SOCKET, SO_LINGER, &tmp, sizeof(tmp));
    }

    int ret = 0;
    struct sockaddr_in address;
    bzero(&address, sizeof(address));  // 初始化address结构体变量中sin_zero字符数组全部置为0
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = htonl(INADDR_ANY);  // htonl()将主机的无符号长整型数转换为网络字节顺序
    address.sin_port = htons(m_port);  // htons将主机无符号短整型数转换为网络字节顺序

    int flag = 1;
    setsockopt(m_listenfd, SOL_SOCKET, SO_REUSEADDR, &flag, sizeof(flag));  // 一般不会立即关闭而经历TIME_WAIT过程，后续想重用socket
    ret = bind(m_listenfd, (struct sockaddr *)&address, sizeof(address));
    assert(ret >= 0);
    ret = listen(m_listenfd, 5);
    assert(ret >= 0);

    //工具类,信号和描述符基础操作
    Utils::u_pipefd = m_pipefd;
    Utils::u_epollfd = m_epollfd;

    utils.init(timer_lst, TIMESLOT);

    //epoll创建内核事件表
    epoll_event events[MAX_EVENT_NUMBER];
    m_epollfd = epoll_create(5);  // 生成epoll专用的文件描述符，其实是在内核申请空间，用来存放你想关注的socket fd上是否发生以及发生了什么事，size就是在epoll fd上能关注的最大socket fd数
    assert(m_epollfd != -1);

    utils.addfd(m_epollfd, m_listenfd, false, m_TRIGMode);
    http_conn::m_epollfd = m_epollfd;

    ret = socketpair(PF_UNIX, SOCK_STREAM, 0, m_pipefd);  // 创建一对套接字进行进程间通信（IPC），参数（协议族，协议，类型，套接字柄对）SOCK_STREAM基于TCP。套接字柄对作用相同，可进行读写双向操作。
    assert(ret != -1);
    utils.setnonblocking(m_pipefd[1]);
    utils.addfd(m_epollfd, m_pipefd[0], false, m_TRIGMode);

    utils.addsig(SIGPIPE, SIG_IGN);  // SIGPIPE 终止进程
    utils.addsig(SIGALRM, utils.sig_handler, false);  // SIGALRM 当进程的计时器到期时，SIGALRM信号交付给进程。
    utils.addsig(SIGTERM, utils.sig_handler, false);

    alarm(TIMESLOT);  // alarm()用来设置信号SIGALRM在经过参数seconds指定的秒数后传送给目前的进程
}

void WebServer::timer(int connfd, struct sockaddr_in client_address)
{
    users[connfd].init(connfd, client_address, m_root, m_SQLVerify, m_TRIGMode, m_close_log, m_user, m_passWord, m_databaseName);

    //初始化client_data数据
    //创建定时器，设置回调函数和超时时间，绑定用户数据，将定时器添加到链表中
    users_timer[connfd].address = client_address;
    users_timer[connfd].sockfd = connfd;
    util_timer *timer = new util_timer;
    timer->user_data = &users_timer[connfd];
    timer->cb_func = cb_func;
    time_t cur = time(NULL);
    timer->expire = cur + 3 * TIMESLOT;
    users_timer[connfd].timer = timer;
    timer_lst.add_timer(timer);
}

//若有数据传输，则将定时器往后延迟3个单位
//并对新的定时器在链表上的位置进行调整
void WebServer::adjust_timer(util_timer *timer)
{
    time_t cur = time(NULL);
    timer->expire = cur + 3 * TIMESLOT;
    timer_lst.adjust_timer(timer);

    LOG_INFO("%s", "adjust timer once");
}

void WebServer::deal_timer(util_timer *timer, int sockfd)
{
    timer->cb_func(&users_timer[sockfd]);
    if (timer)
    {
        timer_lst.del_timer(timer);
    }

    LOG_INFO("close fd %d", users_timer[sockfd].sockfd);
}

bool WebServer::dealclinetdata()
{
    struct sockaddr_in client_address;
    socklen_t client_addrlength = sizeof(client_address);
    if (0 == m_TRIGMode)
    {
        int connfd = accept(m_listenfd, (struct sockaddr *)&client_address, &client_addrlength); 
        // accept()系统调用主要用在基于连接的套接字类型，比如SOCK_STREAM和SOCK_SEQPACKET。它提取出所监听套接字的等待连接队列中第一个连接请求，
        // 创建一个新的套接字，并返回指向该套接字的文件描述符。新建立的套接字不在监听状态，原来所监听的套接字也不受该系统调用的影响。
        if (connfd < 0)
        {
            LOG_ERROR("%s:errno is:%d", "accept error", errno);
            return false;
        }
        if (http_conn::m_user_count >= MAX_FD)
        {
            utils.show_error(connfd, "Internal server busy");
            LOG_ERROR("%s", "Internal server busy");
            return false;
        }
        timer(connfd, client_address);
    }

    else
    {
        while (1)
        {
            int connfd = accept(m_listenfd, (struct sockaddr *)&client_address, &client_addrlength);
            if (connfd < 0)
            {
                LOG_ERROR("%s:errno is:%d", "accept error", errno);
                break;
            }
            if (http_conn::m_user_count >= MAX_FD)
            {
                utils.show_error(connfd, "Internal server busy");
                LOG_ERROR("%s", "Internal server busy");
                break;
            }
            timer(connfd, client_address);
        }
        return false;
    }
    return true;
}

bool WebServer::dealwithsignal(bool& timeout, bool& stop_server)
{
    int ret = 0;
    int sig;
    char signals[1024];
    ret = recv(m_pipefd[0], signals, sizeof(signals), 0);  // 从TCP另一端接受数据，signals为缓冲区，返回接受的字节数。
    if (ret == -1)
    {
        return false;
    }
    else if (ret == 0)
    {
        return false;
    }
    else
    {
        for (int i = 0; i < ret; ++i)
        {
            switch (signals[i])
            {
            case SIGALRM:
            {
                timeout = true;
                break;
            }
            case SIGTERM:
            {
                stop_server = true;
                break;
            }
            }
        }
    }
    return true;
}

void WebServer::dealwithread(int sockfd)
{
    util_timer *timer = users_timer[sockfd].timer;

    //reactor
    if (1 == m_actormodel)
    {
        if (timer)
        {
            adjust_timer(timer);
        }

        //若监测到读事件，将该事件放入请求队列
        m_pool->append(users + sockfd, 0);

        while (true)
        {
            if (1 == users[sockfd].improv)
            {
                if (1 == users[sockfd].timer_flag)
                {
                    deal_timer(timer, sockfd);
                    users[sockfd].timer_flag = 0;
                }
                users[sockfd].improv = 0;
                break;
            }
        }
    }
    else
    {
        //proactor
        if (users[sockfd].read_once())
        {
            LOG_INFO("deal with the client(%s)", inet_ntoa(users[sockfd].get_address()->sin_addr));

            //若监测到读事件，将该事件放入请求队列
            m_pool->append_p(users + sockfd);

            if (timer)
            {
                adjust_timer(timer);
            }
        }
        else
        {
            deal_timer(timer, sockfd);
        }
    }
}

void WebServer::dealwithwrite(int sockfd)
{
    util_timer *timer = users_timer[sockfd].timer;
    //reactor
    if (1 == m_actormodel)
    {
        if (timer)
        {
            adjust_timer(timer);
        }

        m_pool->append(users + sockfd, 1);

        while (true)
        {
            if (1 == users[sockfd].improv)
            {
                if (1 == users[sockfd].timer_flag)
                {
                    deal_timer(timer, sockfd);
                    users[sockfd].timer_flag = 0;
                }
                users[sockfd].improv = 0;
                break;
            }
        }
    }
    else
    {
        //proactor
        if (users[sockfd].write())
        {
            LOG_INFO("send data to the client(%s)", inet_ntoa(users[sockfd].get_address()->sin_addr));

            if (timer)
            {
                adjust_timer(timer);
            }
        }
        else
        {
            deal_timer(timer, sockfd);
        }
    }
}

void WebServer::eventLoop()
{
    bool timeout = false;
    bool stop_server = false;

    while (!stop_server)
    {
        int number = epoll_wait(m_epollfd, events, MAX_EVENT_NUMBER, -1);  // 轮询I/O事件的发生, event 用于回传代处理事件的数组，-1 阻塞，等待I/O事件发生的超时值。
        if (number < 0 && errno != EINTR)
        {
            LOG_ERROR("%s", "epoll failure");
            break;
        }

        for (int i = 0; i < number; i++)
        {
            int sockfd = events[i].data.fd;

            //处理新到的客户连接
            if (sockfd == m_listenfd)
            {
                bool flag = dealclinetdata();
                if (false == flag)
                    continue;
            }
            else if (events[i].events & (EPOLLRDHUP | EPOLLHUP | EPOLLERR))
            {
                //服务器端关闭连接，移除对应的定时器
                util_timer *timer = users_timer[sockfd].timer;
                deal_timer(timer, sockfd);
            }
            //处理信号
            else if ((sockfd == m_pipefd[0]) && (events[i].events & EPOLLIN))
            {
                bool flag = dealwithsignal(timeout, stop_server);
                if (false == flag)
                    continue;
            }
            //处理客户连接上接收到的数据
            else if (events[i].events & EPOLLIN)
            {
                dealwithread(sockfd);
            }
            else if (events[i].events & EPOLLOUT)
            {
                dealwithwrite(sockfd);
            }
        }
        if (timeout)
        {
            utils.timer_handler();

            LOG_INFO("%s", "timer tick");

            timeout = false;
        }
    }
}