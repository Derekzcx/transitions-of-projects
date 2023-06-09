#include"_public.h"
#include"_mysql.h"
/*
    mysql中需要的表（用户表）：T_USERNAME；（用户接口权限表）：T_USERANDNAME
*/

/*******************************************/
CLogFile logfile;       // 服务器程序的运行日志
CTcpServer TcpServer;   // 创建服务端的对象
/******************************************/
pthread_mutex_t mutex = PTHREAD_MUTEX_INITIALIZER;  // 采用宏初始化互斥锁
pthread_cond_t  cond = PTHREAD_COND_INITIALIZER;    // 采用宏初始化条件变量
pthread_spinlock_t spin;                            // 定义一个自旋锁——用于锁定线程id
//线程信息的结构体
struct st_pth_info{
    pthread_t pth_id;       // 线程id
    time_t    last_time;    // 最近一次活动的时间
}
vector<struct st_pth_info> v_pth_id;    // 用于存放全部线程的容器；              
vector<int> sock_queue;                 // 客户端的 socket 队列
// 主程序参数结构体
struct st_arg{
    char connstr[101];      // 数据库的连接参数
    char charset[51];       // 数据库的字符集
    int port;               // web 服务监控的端口
} starg;

pthread_t check_pth_id;      // 监察各线程状态子线程id
pthread_t check_sqlpool_id; // 监察数据库连接子线程id
/********************************************/
 // 进程退出的函数
void EXIT(int sig);  

// 显示帮助提示
void _help(char *argv[]);     

// 将xml数据解析到主程序参数结构体中 
bool _xml_to_arg(char *strxmlbuffer);

// 读取客户端报文（GET）,参数：客户端id，接收buffer，buffer大小，超时时间
int ReadT(const int sock_fd, char *buffer, const int size, const int time_out);

// 从 GET 中获取参数
bool getvalue(const char *buffer, const char *name, char *value, const int len);

//判断url中的用户名和密码，如果不正确返回认证失败的报文
bool Login(connection *conn, const char *buffer, const int sock_fd);

//判断登录用户是否有调用接口的权限，如果没有，返回没有权限的报文
bool Check_Permission(connection *conn, const char *buffer, const int sock_fd);

// 执行接口的 sql 语句，把数据返回给客户端
bool Exec_SQL(connection *conn, const char *buffer, const int sock_fd);

// 子线程工作主函数
void *pth_main(void *arg);  

// 监察 其他子线程工作的的子线程
void *check_pth_main(void *arg);

// 监察 数据库连接池个子连接状态的子线程
void *check_sqlpool_main(void *arg); 

// 线程清理函数
void pth_clearn_up(void *arg);  

/*****************  数据库池管理类 **************************/
class SQL_CONN_POOL{
private:
    struct st_conn{
        connection conn;        // 该连接对象的数据库连接
        pthread_mutex_t mutex;  // 该连接对象的访问互斥锁
        time_t last_time;       // 该连接对象最近一次活动的时间
    } *m_conns;                 // 用指针管理
    
    int m_max_conns;            // 数据库连接池最大尺寸
    int m_time_out;             // 数据库连接超时时间, 单位：秒
    char m_connstr[101];        // 连接参数：ip，用户名，密码，数据库名
    char m_charset[101];        // 数据库的字符集格式
public:
    SQL_CONN_POOL();
    ~SQL_CONN_POOL();
    
    // 初始化数据库连接池，初始化锁，如果数据库连接参数有问题，返回fasle
    bool init(const char *connstr, const char *charset, int max_conns, int time_out);

    // 断开数据库连接，销毁锁，释放数据库连接池的内存空间
    void destroy();

    // 从数据库连接池中获取一个空闲的连接，成功返回数据库连接的地址
    // 如果连接池已用完 或 连接数据库失败，返回空
    connection* get();

    // 归还数据库连接
    bool free(connection *conn);

    //检查数据库连接池，断开空闲的连接，在服务程序中，用一个专用的子线程调用此函数
    void check_sql_pool();
};

SQL_CONN_POOL sql_connpool_messager;//连接池管理类

/********************************************/

int main(int argc, char* argv[]){
    if(argc != 3){ _help(); return -1;}
    // 关闭全部的信号和输入输出
    // 设置信号，在 shell 状态下可用 kill + 进程号杀死进程
    // 但请不要用 kill -9 + 进程号 强行杀死
    CloseIOAndSignal(); signal(SIGINT, EXIT); signal(SIGTERM. EXIT);

    if(logfile.Open(argv[1], "a+")==false){printf("logfile.Open(%s) failed.\n",argv[1]); return -1;}
    //把xml解析到参数 starg 数据结构中
    if(_xml_to_arg(argv[2]) == false) return -1;
    // 服务端初始化
    if(TcpServer.InitServer(starg.port) == false){
        logfile.Write("TcpServer.Initserver(%d) failed.\n",starg.port); return -1;
    }
    //初始化数据库连接池
    if(sql_connpool_messager.init(starg.connstr, starg.charset, 10, 50) == false){
        logfile.Write("sql_connpool_messager.init() failed.\n");return -1;
    }else{//创建检查数据库连接池的线程
        if(pthread_create(&check_sqlpool_id, NULL, check_sqlpool_main, 0) != 0){
            logfile.Write("pthread_create() failed.\n");return -1;
        }
    }
    /************************ 线程创建 *****************************/
    //启动 10 个工作线程，线程数一般比cpu的核数略多
    for(int i=0; i<10; i++){
        struct st_pth_info stpthinfo;
        if(pthread_create(&stpthinfo, NULL, pth_main, (void *)(long)i) != 0){// 创建线程，传入参数为线程编号
            logfile.Write("pthread_create() failed.\n"); return -1;
        }
        stpthinfo.last_time = time(0);  //更新线程活动时间
        v_pth_id.push_back(stpthinfo);  //将线程id加入队列中 
    }
    //检查线程创建
    if(pthread_create(&check_pth_id, NULL, check_pth_main, NULL) != 0){
        logfile.Write("pthread_create(check_pth_main) failed .\n"); return -1;
    }
    //初始化自旋锁
    pthread_spin_init(&spin, PTHREAD_PROCESS_PRIVATE);// 初始化自旋锁，并设置为本进程内子线程间共享

    while(true){
        // 等待客户端的连接请求
        if(TcpServer.Accept() == false){// 服务端阻塞等待客户端连接
            logfile.Write("TcpServer.Accept() failed .\n"); return -1;
        }
        logfile.Write("客户端（%s）已连接。\n", TcpServer.GetIP());
        // 把客户端的socket放入队列，然后发送条件信号
        pthread_mutex_lock(&mutex);             // 加锁
        sock_queue.push_back(TcpServer.m_connfd);//客户端socket入队
        pthread_mutex_unlock(&mutex);           // 解锁
        pthread_cond_signal(&cond);             // 条件变量激活一个子线程
    }
}

// 处理客户端的主函数
// 子线程工作主函数
void *pth_main(void *arg){
    int pth_num = (int)(long)arg;//获取传入的线程id
    pthread_cleanup_push(pth_clearn_up, arg);//线程清理函数入栈, 参数1位一维指针，所以直接传函数名

    pthread_setcanceltype(PTHREAD_CANCEL_ASYNCHRONOUS, NULL);// 设置线程的取消方式是立即取消
    pthread_detach(pthread_self());                         // 设置线程为分离，无需主线程调动join等待资源回收，线程结束系统自动回收

    int connfd;                     // 客户端的socket
    char str_recv_buffer[1024];     // 接收客户端请求报文的buffer
    char str_send_buffer[1024];     // 向客户端发送回应报文的buffer
/*//包含在 include<sys/time.h>中
struct timeval {
               time_t      tv_sec;    // seconds 
               suseconds_t tv_usec;   //  microseconds
           };
*/
    while(true){
        pthread_mutex_lock(&mutex);//对socket的缓存队列加锁
        //如果缓存队列为空, 等待，用while防止条件变量虚假唤醒
        while(sock_queue.size() == 0){
            struct timeval now;         //内置结构体，用于获取系统时间
            gettimeofday(&now, NULL);   //获取当前时间。
            now.tv_sec = now.tv_sec+20; //取20秒后的时间
            pthread_cond_timedwait(&cond, &mutex, (struct timespec*)&now);//阻塞，先解锁，再等待条件变量唤醒，设置等待的超时时间为20秒
            v_pth_id[pth_num].last_time = time(0); 
            //情况1：20秒后没socket加入，就停止阻塞并加锁，更新时间, while 继续阻塞
            //情况2：有连接socket加入，阻塞停止并加锁，跳出循环
        }
    }
    // 从socket缓存队列中获取第一条记录，然后删除该记录
    connfd = sock_queue[0];
    sock_queue.erase(sock_queue.begin());

    pthread_mutex_unlock(&mutex);// 解锁

    /************************* 进行业务处理 *******************************/
    logfile.Write("pth_id = %lu(num=%d), connfd=%d", pthread_self(), pth_num, connfd);
    //读取客户端的报文，如果超时或失败，关闭客户端的socket，继续回到循环
    memset(str_recv_buffer, 0, sizeof(str_recv_buffer));
    if(ReadT(connfd, str_recv_buffer, sizeof(str_recv_buffer), 30000) <= 0){
        close(connfd); continue;// 读取失败则关闭连接，并继续服务循环
    }
    //如果不是 GET 请求报文，则不处理，关闭客户端的 socket，继续循环
    if(strncmp(str_recv_buffer, "GET", 3) != 0){
        close(connfd); continue;
    }
    //收到请求报文
    logfile.Write("%s \n",str_recv_buffer);
    //从数据库连接池中获取一个连接
    connection *conn = sql_connpool_messager.get();//
    //如果得到的连接为空，需要想客户端发送内部错误提示，关闭客户端的socket，继续循环
    if(conn == NULL){
        memset(str_send_buffer, 0, sizeof(str_send_buffer));
        sprintf(str_send_buffer,\
                "HTTP/1.1 200 ok\r\n"\
                "Server:webserver\r\n"\
                "Content-Type:text/html;charset=utf-8\r\n\r\n"\
                "<retcode>-1</retcode><message>internal error.</message>");
        Writen(connfd, str_send_buffer, strlen(str_send_buffer));
        close(connfd);
        continue;
    }
    //连接没问题，则开始处理报文
    //（1）判断URL中用户名和密码，如果不正确，返回认证失败的响应报文，关闭客户端的socket，继续回到循环
    if(Login(conn, str_recv_buffer, connfd) == false){
        sql_connpool_messager.free(conn);//退回数据库连接
        continue;
    }
    //（2）判断用户是否有调用接口的权限，如果没有，返回没有权限的响应报文，关闭客户端的socket，继续回到循环
    if(Check_Permission(conn, str_recv_buffer, connfd) == false){
        sql_connpool_messager.free(conn);//退回数据库连接
        continue;
    } 
    //（3）先把响应报文头部发送给客户端
    memset(str_send_buffer, 0, sizeof(str_send_buffer));
    sprintf(str_send_buffer,\
            "HTTP/1.1 200 ok\r\n"\
            "Server: webserver\r\n"\
            "Content-Type:text/html;charset=utf-8\r\n\r\n")
    Writen(connfd, str_send_buffer, strlen(str_send_buffer));
    // （4）再执行接口的sql语句，把数据返回给客户端
    if(Exec_SQL(conn, str_send_buffer, connfd) == false){//执行失败
        sql_connpool_messager.free(conn);   //退回数据库连接
        close(connfd);// 因为采用的是短连接，所以执行
        continue;
    }
    // 因为采用的是短连接策略，服务端执行完客户端的请求后就断开
    sql_connpool_messager.free(conn);
    close(connfd);

    pthread_cleanup_pop(1);// 出栈时执行线程清理函数
}

// 监察 数据库连接池个子连接状态的子线程
void *check_sqlpool_main(void *arg){
    while(true){
        sql_connpool_messager.check_sql_pool(); sleep(30);// 每30s调用一次检查
    }
}

// 监察 其他子线程工作的的子线程
void *check_pth_main(void *arg){
    while(true){
        //遍历工作线程结构体的容器，检查每个工作线程是否超时
        for(int i=0; i<v_pth_id.size(); i++){
            // 工作线程超时时间设计为20s，这采用25s判断是否超时，足够
            if((time(0) - v_pth_id[i].last_time) > 25){
                // 已超时
                logfile.Write("thread %d(%lu) timeout(%d).\n", i, v_pth_id[i].pth_id, time(0)-v_pth_id[i].last_time);
                // 取消已超时的工作时间
                pthread_cancel(v_pth_id[i].pth_id);
                // 重新创建工作线程
                if(pthread_create(&v_pth_id[i].pth_id, NULL, pth_main, (void*)(long)i) != 0){
                    logfile.Write("pthread_create() failed \n."); EXIT(-1);
                }
                v_pth_id[i].last_time = time(0);//更新活动时间
            }
        }
    }
}

// 线程清理函数
void pth_clearn_up(void *arg){
    pthread_mutex_unlock(&mutex);//先退共享区解锁， 不然无法退出

    //本线程的结构体从存放线程结构体的容器中删除
    pthread_spin_lock(&spin);//自旋锁对容器进行删除操作，因为删除等待不久，可用自旋锁不断尝试解锁
    for(int i=0; i<v_pth_id.size(); i++){
        if(pthread_equal(pthread_self(), v_pth_id[i].pth_id)){// 线程id判断函数，若id相等，函数返回非0，否则返回0
            v_pth_id.erase(v_pth_id.begin()+i);
            break;
        }
    }
    pthread_spin_unlock(&spin);
    logfile.Write("线程%d(%lu)退出.\n", (int)(long)arg, pthread_self());
}

 // 进程退出的函数
void EXIT(int sig){
    //以下代码是为了防止信号处理函数在执行的过程中被信号中断
    signal(SIGINIT, SIG_IGN); signal(SIGTERM, SIG_IGN);//忽略ctrl+c -信号2 ，忽略kill + 进程号默认 -信号15
    logfile.Write("进程退出，sig=%d.\n", sig);
    TcpServer.CloseListen();//关闭监听的socket

    // 取消全部的线程
    pthread_spin_lock(&spin);//自旋锁
    for(int i=0; i<v_pth_id.size(); i++){
        pthread_cancel(v_pth_id[i].pth_id);
    }
    pthread_spin_unlock(&spin);
    sleep(1);//让子线程有足够的时间退出

    pthread_cancel(check_pth_id);//取消监控线程；
    pthread_cancel(check_sqlpool_id);//取消对数据库的监控线程

    pthread_mutex_destroy(&mutex);
    pthread_spin_destroy(&spin);
    pthread_cond_destroy(&cond);
    exit(0);//退出
}

void _help(char *argv[]){
    printf("Using:./multi_webserver_sql logfilename xmlbuffer\n\n");
    printf("Sample:/home/zcx01/project/tools1/bin/procctcl 10"\
     "/home/zcx01/project/test/multi_webserver_sql"\
    "/home/zcx01/project/log/webserver/web.log"\
    "\"<connstr>127.0.0.1,root,zcx123,mysql</connstr>"\
    "<charset>utf8</charset>"\
    "<port>8080</port>\"\n\n");

    printf("本程序是数据总线的服务端程序，为数据中心提供http协议的数据访问接口。\n");                                                                                                                                           
    printf("logfilename 本程序运行的日志文件。\n");                                                                                                                                                                             
    printf("xmlbuffer   本程序运行的参数，用xml表示，具体如下：\n\n");                                                                                                                                                         
   
   printf("connstr     数据库的连接参数，格式:127.0.0.1,root,zcx123,mysql\n");                                                                                                                                             
   printf("charset     数据库的字符集，这个参数要与数据源数据库保持一致，否则会出现中文乱码的情况。\n");                                                                                                                       
   printf("port        web服务监听的端口。\n\n");  
}

// 读取客户端的报文,涉及IO复用知识
// 参数1 :对方sockid
// 参数2 :接收数据的buffer
// 参数3：buffer的大小
// 参数4：指定参数的时间
int ReadT(const int sock_id, char *buffer, const int size, const int time_out)
{
  if(time_out > 0){
      struct pollfd fds;
      fds.fd = sock_id;
      fds.events = POLLIN;
      int iret = poll(&fds, 1, time_out*1000);
      if(iret <=0) return iret;
  }
  return recv(sock_id,buffer,size,0);
}

// 将xml数据解析到主程序参数结构体中 
bool _xml_to_arg(char *strxmlbuffer){
   memset(&starg,0,sizeof(struct st_arg));                                                                                                                                                                                                                                                                                                                                                  |||     init [connpool]
   GetXMLBuffer(strxmlbuffer,"connstr",starg.connstr,100);                                                                                                                                     
   if (strlen(starg.connstr)==0) { logfile.Write("connstr is null.\n"); return false; }                                                                                                      
                                                                                                                                                                                             
   GetXMLBuffer(strxmlbuffer,"charset",starg.charset,50);                                                                                                                                
   if (strlen(starg.charset)==0) { logfile.Write("charset is null.\n"); return false; }                                                                                                  
                                                                                                                                                                                           
   GetXMLBuffer(strxmlbuffer,"port",&starg.port);                                                                                                                                          
   if (starg.port==0) { logfile.Write("port is null.\n"); return false; }                                                                                                                     
                                                                                                                                                                                                                            
   return true;                                                             
}
// 从 HTTP 中的 GET 中获取参数， buffer:get的url，name:参数名字，value：接收对象，len:接收长度+1
bool getvalue(const char *buffer, const char *name, char *value, const int len){
    value[0] = 0;
    char *start, *end;
    start = end = NULL;
    start = strstr((char*)buffer, (char*)name);//寻找子串
    if(start == NULL) return false;
    
    end = strstr(start, "&");// 检查是否有多个参数
    if(end == NULL) end = strstr(start, " "); // 只有单个参数时
    if(end == NULL) return false;
    
    int tmp_len = end - (start + strlen(name) + 1);
    if(tmp_len > len) tmp_len = len;

    strcnpy(value, start + strlen(name) + 1, tmp_len); // name=, start指向n
    
    value[tmp_len] = 0;//为[] 转 char* 预留\0的位置
    
    return true;
}

//判断url中的用户名和密码，如果不正确返回认证失败的报文,conn为数据库连接，buffer为get的url，sock_fd为与客户端的连接 
bool Login(connection *conn, const char *buffer, const int sock_fd){
    char username[31], passwd[31];
    //从 GET 中获取 对应的子串
    getvalue(buffer, "username", username, 30);
    getvalue(buffer, "passwd", 30);
    
    // 查询 T_USERNAME 表，判断用户名和密码是否存在；
    sqlstatement stmt;
    stmt.connect(conn);
    stmt.prepare("select count(*) from T_USERNAME where username=:1 and passwd=:2");
    stmt.bindin(1, username, 30);
    stmt.bindin(2, passwd, 30);
    int icount = 0;
    stmt.bindout(1, &icount);//绑定输出
    stmt.execute();//执行
    stmt.next();//输出一行查询的数据

    if(icount == 0){ // 说明没注册这个用户，则需要返回认证失败的响应报文
        char strbuffer[256];
        memset(strbuffer, 0, sizeof(strbuffer));

        sprintf(strbuffer,\
                "HTTP/1.1 200 ok\r\n"\
                "Server: webserver\r\n"\
                "Contect-Type: text/html;charset=utf-8\r\n\r\n"\
                "<retcode>-1</retcode><message>username or passwd is invaild</message>");
        Writen(sock_fd, strbuffer, strlen(strbuffer));
        return false;
    }
    return true;
}

//判断登录用户是否有调用接口的权限，如果没有，返回没有权限的报文
bool Check_Permission(connection *conn, const char *buffer, const int sock_fd){
    char username[31], intername[30];

    getvalue(buffer, "username", username, 30);     // 获取用户名
    getvalue(buffer, "intername", intername, 30);   // 获取接口名

    sqlstatement stmt;
    stmt.connect(conn);
    stmt.prepare("select count(*) from T_USERANDNAME where username=:1 and intername=:2");
    stmt.bindin(1, username, 30);
    stmt.bindin(2, intername, 30);
    int tmp_count = 0;
    stmt.bindout(1, &tmp_count);
    stmt.execute();
    stmt.next();

    if(tmp_count != 1){
        char strbuffer[256];
        memset(strbuffer, 0, sizeof(strbuffer));
        sprintf(strbuffer, \
                "HTTP/1.1 200 ok\r\n"\
                "Server: Webserver\r\n"\
                "Content-Type: text/html;charset=utf-8\r\n\r\n"\
                "<retcode>-1</retcode><message>permission denied</message>");
        Writen(sock_fd, strbuffer, strlen(strbuffer));
        return false;
    }
    return true;
}

// ********************执行接口的 sql 语句，把数据返回给客户端********
bool Exec_SQL(connection *conn, const char *buffer, const int sock_fd){
    //从请求报文中解析接口名
    char intername[30];
    memset(intername, 0, sizeof(intername));
    getvalue(buffer, "intername", intername, 30);//  获取接口名，这个接口名就像是该用户能够调用的sql查询语句

    // 从接口参数配置表中 
    char select_sql[1001], col_str[301], bindin[301];
    memset(select_sql, 0, sizeof(select_sql)); // sql查询语句
    memset(col_str, 0, sizeof(col_str));       // 查询输出的字段名
    memset(bindin, 0, sizeof(bindin));         // 接口参数
    
    sqlstatement stmt;
    stmt.connect(conn);
    stmt.prepare("select selectsql, colstr, bindin from T_INTERRCFG where intername=:1");
    stmt.bindin(1, intername, 30);
    stmt.bindout(1, select_sql, 1000);
    stmt.bindout(2, col_str, 300);
    stmt.bindout(3, bindin, 300);
    stmt.execute();
    stmt.next();

    //准备对应接口的查询//select obtid, ddatetime, t, p, u, wd, wf, r, vis, upttime from T_ZHOBTMIND ORDER BY obtid
    stmt.prepare(select_sql);
    // 一般采用 一个数据结构的容器，通过while 不断next去获取数据到容器内
    /***************************************/
    /*
        获取数据库数据， 将数据发送给客户端
    */
    /*************************************/
}




/***************** 数据库连接池管理类的函数 ********************/
SQL_CONN_POOL::SQL_CONN_POOL(){
    m_max_conns = 0;
    m_time_out = 0;
    memset(m_connstr, 0, sizeof(m_connstr));
    memset(m_charset, 0, sizeof(m_charset));
}
SQL_CONN_POOL::~SQL_CONN_POOL(){
    destroy();
}

// 初始化数据库连接池，初始化锁，如果数据库连接参数有问题，返回fasle
bool SQL_CONN_POOL::init(const char *connstr, const char *charset, int max_conns, int time_out){
    //尝试连接数据库，验证数据库连接参数是否正确
    connection conn;
    //mysql 数据库：conn.connecttodb("127.0.0.1,root,zcx123,mysql,3306","utf8") 
    if(conn.connecttodb(connstr, charset) != 0){
        printf("连接数据库失败。失败参数：%s\n错误提示：%s\n",conn.m_cda.message); return false;
    }
    conn.disconnect();//先断开连接

    strncpy(m_connstr, connstr, 100);//字符串复制,保存连接参数
    strncpy(m_charset, charset, 100);//字符串复制，保存字符集格式参数
    m_max_conns = max_conns;
    m_time_out = time_out;

    //分配数据库连接池内存空间，并初始化每一个连接
    m_conns = new struct st_conn[m_max_conns];
    for(int i=0; i<m_max_conns; i++){
        pthread_mutex_init(&m_conns[i].mutex, 0);   //初始化锁
        m_conns[i].last_time = 0;                   //初始化最近一次获得的时间
    }
    return true;
}

//断开数据库连接，销毁锁，释放数据库连接池的内存空间
void SQL_CONN_POOL::destroy(){
    for(int i=0; i<m_max_conns; i++){
        m_conns[i].conn.disconnect();               //断开连接
        pthread_mutex_destroy(&m_conns[i].mutex);   //销毁该连接的锁
    }
    delete []m_conns;//销毁内存
    //重新初始化各参数
    m_conns = NULL;// 连接池管理指针置空
    memset(m_connstr, 0, sizeof(m_connstr));
    memset(m_charset, 0, sizeof(m_charset));
    m_max_conns = 0;
    m_time_out = 0;
}

// 1.从数据库连接池中寻找一个空闲的、已连接好的connection，如果找到了，返回它的地址
// 2.如果没找到，在连接池中找一个未连接数据库的connection，连接数据库，如果成功，返回connection的地址
// 3.如果第2步找到了未连接数据库的connection，但是，连接数据库失败，返回空
// 4.如果第2步没找到未连接数据库的connection，表示数据库连接池已用完，也返回空
connection * SQL_CONN_POOL::get(){
    int pos = -1;       //用于记录第一个未连接数据库的数组位置
    for(int i=0; i<m_max_conns; i++){//遍历数据库池
        if(pthread_mutex_trylock(&m_conns[i].mutex) == 0){//尝试加锁
            // 第1种情况
            if(m_conns[i].last_time > 0){       //属于第1种情况，空闲已连接的数据库连接
                printf("取到连接%d.\n",i);
                m_conns[i].last_time = time(0); //获取当前时间
                return &m_conns[i].conn;        //返回对应的数据库连接的地址
            }
            // 第2中情况
            if(pos == -1) pos = i;              //记录第一个未连接数据库的数组位置
            else pthread_mutex_unlock(&m_conns[i].mutex); //释放锁
        }
        if(pos == -1){//如果没有连接池已用完，返回空
            printf("连接池已用完。\n"); return NULL;
        }
        printf("新连接%d.\n", pos);//连接池没有用完，让m_conns[pos].conn连上数据库。
        // 把pos位置上的连接连接数据库
        if(m_conns[pos].conn.connecttodb(m_connstr, m_charset) != 0){
            printf("\n");//如果连接数据库失败，释放锁，返回空
            pthread_mutex_unlock(&m_conns[pos].mutex);//释放锁
            return NULL;
        }
        m_conns[pos].last_time = time(0);//更新连接的活动时间点
        return &m_conns[pos].conn;
    }
}

// 归还数据库连接，只是释放锁，但不断开连接
bool SQL_CONN_POOL::free(connection *conn){
    for(int i=0; i<m_max_conns; i++){
        if(&m_conns[i].conn == conn){
            printf("归还%d\n", i);
            //把数据库连接的使用时间设置为当前时间，以方便下次获取时直接使用
            m_conns[i].last_time = time(0);
            pthread_mutex_unlock(&m_conns[i].mutex);//释放锁
            return true;
        }
    }
    return false;//释放锁失败
}

// 检查连接池，断开空闲的连接
void SQL_CONN_POOL::check_sql_pool(){
    for(int i=0; i<m_max_conns; i++){
        if(pthread_mutex_trylock(&m_conns[i].mutex) == 0){// 尝试加锁，加锁成功返回0
            if(m_conns[i].last_time > 0){// 满足该条件则表示该是已连接的连接对象
                // 检查是否连接超时
                if((time(0) - m_conns[i].last_time) > m_time_out){
                    printf("连接%d已超时。\n", i);
                    m_conns[i].conn.disconnect();//断开连接
                    m_conns[i].last_time = 0;   // 初始化连接时间，使该连接表示为未连接状态
                }else{
                    //如果没有超时，执行一次sql，检验连接是否有效，如果无效，则断开它
                    //如果网络断开了， 或者数据可重启了，那么需要重新连接数据库，在这里，只需要断开连接就行了
                    //重连的操作交给get()函数
                    if(m_conns[i].conn.execute("select * from dual") != 0){// -------------------------------------sql
                        printf("连接 %d 已故障。\n",i);
                        m_conns[i].conn.disconnect();//断开数据库连接。
                        m_conns[i].last_time = 0;    //重新初始化连接
                    }                        
                }
            }
        }
        pthread_mutex_unlock(&m_conns[i].mutex);//释放锁
    }
    // 如果尝试加锁失败，表示数据库连接正在使用中，不必检查
}