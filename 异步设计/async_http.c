#include <stdio.h>
#include <stdlib.h>
#include <string.h>
 
#include <unistd.h>
#include <time.h>
#include <fcntl.h>
#include <errno.h>
 
#include <sys/types.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <unistd.h>       /* close */
#include <netdb.h> 
 
#include <sys/epoll.h>
#include <pthread.h>
 
 
#define HTTP_VERSION    "HTTP/1.1"
#define USER_AGENT      "User-Agent: Mozilla/5.0 (Windows NT 5.1; rv:10.0.2) Gecko/20100101 Firefox/10.0.2\r\n"
#define ENCODE_TYPE     "Accept: text/html,application/xhtml+xml,application/xml;q=0.9,*/*;q=0.8\r\n"
#define CONNECTION_TYPE "Connection: close\r\n"
 
 
 
#define BUFFER_SIZE     4096
 
 
char *host_to_ip(const char *hostname) {
    /**
     gethostbyname是线程不安全的（不可重入）里面含有全局变量
     gethostbyname_r是线程安全的单只支持Ipv4  
     getaddrinfo是线程安全支持ipv6
     */
    struct hostent *host_entry = gethostbyname(hostname);
    if (host_entry) {
        return inet_ntoa(*(struct in_addr*)*host_entry->h_addr_list);
    } else {
        return NULL;
    }
}
 
 
#define ASYNC_CLIENT_NUM        1024
#define HOSTNAME_LENGTH         128
 
typedef void (*async_result_cb)(const char *hostname, const char *result);
 
 
struct ep_arg {
    int sockfd;
    char hostname[HOSTNAME_LENGTH];
    async_result_cb cb;
};
 
 
struct async_context {
    int epfd;
    pthread_t thread_id;
};
 
struct http_request {
    char *hostname;
    char *resource;
};
 
 
int http_async_client_commit(struct async_context *ctx, const char *hostname, const char *resource, async_result_cb cb) {

    char *ip = host_to_ip(hostname);
    if (ip == NULL) return -1;
     
    int sockfd = socket(AF_INET, SOCK_STREAM, 0);
 
    struct sockaddr_in sin = {0};
    sin.sin_addr.s_addr = inet_addr(ip);
    sin.sin_port = htons(80);
    sin.sin_family = AF_INET;
 
    if (-1 == connect(sockfd, (struct sockaddr*)&sin, sizeof(struct sockaddr_in))) {
        return -1;
    }
 
    fcntl(sockfd, F_SETFL, O_NONBLOCK);
 
    char buffer[BUFFER_SIZE] = {0};
     
    int len = sprintf(buffer, 
"GET %s %s\r\n\
Host: %s\r\n\
%s\r\n\
\r\n",
         resource, HTTP_VERSION,
         hostname,
         CONNECTION_TYPE
         );
 
    printf("request buffer:%s\n", buffer);
    int slen = send(sockfd, buffer, strlen(buffer), 0);
     
    struct ep_arg *eparg = (struct ep_arg*)calloc(1, sizeof(struct ep_arg));
    if (eparg == NULL) return -1;
    eparg->sockfd = sockfd;
    eparg->cb = cb;
 
    struct epoll_event ev;
    ev.data.ptr = eparg;
    ev.events = EPOLLIN;
 
    int ret = epoll_ctl(ctx->epfd, EPOLL_CTL_ADD, sockfd, &ev); 
 
    return ret;
    // return 0;
 
}
 
static void *http_async_client_callback(void *arg) {
 
    struct async_context *ctx = (struct async_context*)arg;
    int epfd = ctx->epfd;
 
    while (1) {
 
        struct epoll_event events[ASYNC_CLIENT_NUM] = {0};
 
        int nready = epoll_wait(epfd, events, ASYNC_CLIENT_NUM, -1);
        if (nready < 0) {
            // printf("nready=%d, errno[%s]\n",nready, strerror(errno));
            if (errno == EINTR || errno == EAGAIN) {
                continue;
            } else {
                 break;
            }
        } else if (nready == 0) {
            continue;
        }
 
        printf("nready:%d\n", nready);
        int i = 0;
        for (i = 0;i < nready;i ++) {
 
            struct ep_arg *data = (struct ep_arg*)events[i].data.ptr;
            int sockfd = data->sockfd;
             
            char buffer[BUFFER_SIZE] = {0};
            struct sockaddr_in addr;
            size_t addr_len = sizeof(struct sockaddr_in);
            int n = recv(sockfd, buffer, BUFFER_SIZE, 0);
 
            data->cb(data->hostname, buffer); //call cb
             
            int ret = epoll_ctl(epfd, EPOLL_CTL_DEL, sockfd, NULL);
            //printf("epoll_ctl DEL --> sockfd:%d\n", sockfd);
 
            close(sockfd); /////
 
            free(data);
 
        }
         
    }
 
}
 
struct async_context *http_async_client_init(void) {
 
    int epfd = epoll_create(1); // 
    if (epfd < 0) return NULL;
 
    struct async_context *ctx = calloc(1, sizeof(struct async_context));
    if (ctx == NULL) {
        close(epfd);
        return NULL;
    }
    ctx->epfd = epfd;
 
    int ret = pthread_create(&ctx->thread_id, NULL, http_async_client_callback, ctx);
    if (ret) {
        perror("pthread_create");
        return NULL;
    }

    /**
    线程一个个时间片调度的
    创建完子线程后 是主线程先执行还是主线程先执行呢？
    一般是主线程，因为创建完后会加入数据队列等待被调度，所以一般主线程先执行
    子线程先执行的情况也有，就是子线程刚好创建完了，此时刚好子线程时间片到了然后切换。
    */
    usleep(1); 
 
    return ctx;
 
}
 
 /**
 这里只是用来释放资源的并没有阻塞的功能 
 根据POSIX标准，pthread_join()、pthread_testcancel()、pthread_cond_wait()、pthread_cond_timedwait()、sem_wait()、sigwait()等函数以及
read()、write()等会引起阻塞的系统调用都是Cancelation-point。
这些都是取消点，

注意：
pthread_cancel(tid); 
pthread_join(tid,&rval);   
因为pthread_join也是取消点所以这里会导致pthread_join不阻塞继续运行。
 */
int http_async_client_uninit(struct async_context *ctx) {
 
    pthread_cancel(ctx->thread_id);
    close(ctx->epfd);
    // pthread_join(ctx->thread_id, NULL);

    return 0;
}
 
struct http_request reqs[] = {
    {"api.seniverse.com", "/v3/weather/now.json?key=0pyd8z7jouficcil&location=beijing&language=zh-Hans&unit=c" },
    {"api.seniverse.com", "/v3/weather/now.json?key=0pyd8z7jouficcil&location=changsha&language=zh-Hans&unit=c" },
    {"api.seniverse.com", "/v3/weather/now.json?key=0pyd8z7jouficcil&location=shenzhen&language=zh-Hans&unit=c" },
    {"api.seniverse.com", "/v3/weather/now.json?key=0pyd8z7jouficcil&location=shanghai&language=zh-Hans&unit=c" },
    {"api.seniverse.com", "/v3/weather/now.json?key=0pyd8z7jouficcil&location=tianjin&language=zh-Hans&unit=c" },
    {"api.seniverse.com", "/v3/weather/now.json?key=0pyd8z7jouficcil&location=wuhan&language=zh-Hans&unit=c" },
    {"api.seniverse.com", "/v3/weather/now.json?key=0pyd8z7jouficcil&location=hefei&language=zh-Hans&unit=c" },
    {"api.seniverse.com", "/v3/weather/now.json?key=0pyd8z7jouficcil&location=hangzhou&language=zh-Hans&unit=c" },
    {"api.seniverse.com", "/v3/weather/now.json?key=0pyd8z7jouficcil&location=nanjing&language=zh-Hans&unit=c" },
    {"api.seniverse.com", "/v3/weather/now.json?key=0pyd8z7jouficcil&location=jinan&language=zh-Hans&unit=c" },
    {"api.seniverse.com", "/v3/weather/now.json?key=0pyd8z7jouficcil&location=taiyuan&language=zh-Hans&unit=c" },
    {"api.seniverse.com", "/v3/weather/now.json?key=0pyd8z7jouficcil&location=wuxi&language=zh-Hans&unit=c" },
    {"api.seniverse.com", "/v3/weather/now.json?key=0pyd8z7jouficcil&location=suzhou&language=zh-Hans&unit=c" },
};
 
 
static void http_async_client_result_callback(const char *hostname, const char *result) {
     
    printf("hostname:%s, result:%s\n\n\n\n", hostname, result);
    fflush(stdout);
}
 
 
 
int main(int argc, char *argv[]) {

    struct async_context *ctx = http_async_client_init();
    if (ctx == NULL) return -1;
 
    int count = sizeof(reqs) / sizeof(reqs[0]);
    int i = 0;
    for (i = 0;i < count;i ++) {
        http_async_client_commit(ctx, reqs[i].hostname, reqs[i].resource, http_async_client_result_callback);
    }
 

    printf("finished commit\n");
 
    getchar();
    /* 这里放到getchar()后面是为了防止close(ctx->epfd); 从而让epfd无效导致没完成commit */
    http_async_client_uninit(ctx);

}