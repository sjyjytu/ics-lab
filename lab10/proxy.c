/*
 * proxy.c - ICS Web proxy
 *
 *
 */

#include "csapp.h"
#include <stdarg.h>
#include <sys/select.h>

/*
 * Function prototypes
 */
int parse_uri(char *uri, char *target_addr, char *path, char *port);
void format_log_entry(char *logstring, struct sockaddr_in *sockaddr, char *uri, size_t size);
ssize_t Rio_readnb_w(rio_t *rp, void *usrbuf, size_t n);
ssize_t Rio_readlineb_w(rio_t *rp, void *usrbuf, size_t maxlen);
ssize_t Rio_writen_w(int fd, void *usrbuf, size_t n);
ssize_t rio_read_w(rio_t *rp, char *usrbuf, size_t n);
void *thread(void *vargp);
struct args
{
    int fd;
    struct sockaddr_in clientaddr;
};

void sigpipe_handler(int sig)
{
    printf("sigpipe handled %d\n", sig);
    return;
}

sem_t mutex, mutex2;
int main(int argc, char **argv)
{
    /* Check arguments */
    if (argc != 2)
    {
        fprintf(stderr, "Usage: %s <port number>\n", argv[0]);
        exit(0);
    }
    Signal(SIGPIPE, sigpipe_handler);
    Sem_init(&mutex, 0, 1);
    Sem_init(&mutex2, 0, 1);

    /* 监听参数port指定端口的请求，充当server */
    int listenfd;
    pthread_t tid;
    socklen_t clientlen;
    listenfd = Open_listenfd(argv[1]);
    clientlen = sizeof(struct sockaddr_storage);
    while (1)
    {
        struct args *p = (struct args *)Malloc(sizeof(struct args));
        /* 获取请求发送者，即client，的hostname和port */
        p->fd = accept(listenfd, (SA *)&p->clientaddr, &clientlen);
        if (p->fd < 0)
        {
            printf("accept error\n");
            Free(p);
            continue;
        }
        Pthread_create(&tid, NULL, thread, p);
    }
    Close(listenfd);
    exit(0);
}

void *thread(void *vargp)
{
    Signal(SIGPIPE, sigpipe_handler);
    Pthread_detach(Pthread_self());
    struct args *p = (struct args *)vargp;
    int connfd = p->fd;
    struct sockaddr_in clientaddr = p->clientaddr;
    Free(p);
    char buf_request[MAXBUF], buf_response[MAXBUF], buf_request_content[MAXBUF], method[MAXLINE], uri[MAXLINE], version[MAXLINE];
    char server_hostname[MAXLINE], server_pathname[MAXLINE], server_port[MAXLINE];
    rio_t rio_request, rio_response;
    int proxy_clientfd;
    int responseSize = 0, n = 0;
    int content_size = 0;
    Rio_readinitb(&rio_request, connfd);
    if (Rio_readlineb_w(&rio_request, buf_request, MAXLINE) <= 0)
    {
        Close(connfd);
        return NULL;
    }
    sscanf(buf_request, "%s %s %s", method, uri, version);
    if (parse_uri(uri, server_hostname, server_pathname, server_port) == -1)
    {
        printf("parse uri:%s error\n", uri);
        Close(connfd);
        return NULL;
    }

    /* 从客户端读取请求头，向目标server发请求，充当client */
    sprintf(buf_request_content, "%s %s %s\r\n", method, server_pathname, version);
    //从客户端读取请求的头
    do
    {
        //请求头部
        buf_request[0] = '\0';
        if ((n = Rio_readlineb_w(&rio_request, buf_request, MAXLINE)) <= 0)
        {
            Close(connfd);
            return NULL;
        }
        strcat(buf_request_content, buf_request);
        if (strstr(buf_request, "Content-Length:"))
            sscanf(buf_request, "Content-Length: %d\r\n", &content_size);
    } while (strcmp(buf_request, "\r\n"));

    //连接server端
    P(&mutex2);
    if ((proxy_clientfd = open_clientfd(server_hostname, server_port)) < 0)
    {
        printf("Open_clientfd error\n");
        Close(connfd);
        V(&mutex2);
        return NULL;
    }
    V(&mutex2);

    //将请求头发给server
    if (Rio_writen_w(proxy_clientfd, buf_request_content, strlen(buf_request_content)) != strlen(buf_request_content))
    {
        printf("write head to server error\n");
        Close(proxy_clientfd);
        Close(connfd);
        return NULL;
    }

    //读取(POST)请求的内容并转发给server
    if ((n = Rio_readnb_w(&rio_request, buf_request_content, content_size)) == content_size)
    {
        // POST请求的内容
        if (Rio_writen_w(proxy_clientfd, buf_request_content, n) != n)
        {
            printf("write to client error\n");
            Close(proxy_clientfd);
            Close(connfd);
            return NULL;
        }
    }
    else
    {
        Close(proxy_clientfd);
        Close(connfd);
        return NULL;
    }
    
    /* 从server读数据，并向client写回*/
    content_size = 0;
    Rio_readinitb(&rio_response, proxy_clientfd);
    //读server头并写回client
    do
    {
        if ((n = Rio_readlineb_w(&rio_response, buf_response, MAXLINE)) <= 0)
        {

            Close(proxy_clientfd);
            Close(connfd);
            return NULL;
        }
        if (rio_writen(connfd, buf_response, n) != n)
        {
            Close(proxy_clientfd);
            Close(connfd);
            return NULL;
        }
        responseSize += n;
        if (strstr(buf_response, "Content-Length:"))
            sscanf(buf_response, "Content-Length: %d\r\n", &content_size);
    } while (strcmp(buf_response, "\r\n"));

    //读server的body并写回
    while (content_size > 0)
    {
        n = rio_read_w(&rio_response, buf_response, (content_size < MAXLINE - 1) ? content_size : MAXLINE - 1);
        if (n <= 0)
        {
            break;
        }
        if (Rio_writen_w(connfd, buf_response, n) != n)
        {
            Close(proxy_clientfd);
            Close(connfd);
            return NULL;
        }
        responseSize += n;
        content_size -= n;
    }

    /* 记录日志 */
    char logstring[MAXLINE];
    P(&mutex);
    format_log_entry(logstring, &clientaddr, uri, responseSize);
    printf("%s\n", logstring);
    V(&mutex);
    Close(proxy_clientfd);
    Close(connfd);
    return NULL;
}

/*
 * parse_uri - URI parser
 *
 * Given a URI from an HTTP proxy GET request (i.e., a URL), extract
 * the host name, path name, and port.  The memory for hostname and
 * pathname must already be allocated and should be at least MAXLINE
 * bytes. Return -1 if there are any problems.
 */
int parse_uri(char *uri, char *hostname, char *pathname, char *port)
{
    char *hostbegin;
    char *hostend;
    char *pathbegin;
    int len;

    if (strncasecmp(uri, "http://", 7) != 0)
    {
        hostname[0] = '\0';
        return -1;
    }

    /* Extract the host name */
    hostbegin = uri + 7;
    hostend = strpbrk(hostbegin, " :/\r\n\0");
    if (hostend == NULL)
        return -1;
    len = hostend - hostbegin;
    strncpy(hostname, hostbegin, len);
    hostname[len] = '\0';

    /* Extract the port number */
    if (*hostend == ':')
    {
        char *p = hostend + 1;
        while (isdigit(*p))
            *port++ = *p++;
        *port = '\0';
    }
    else
    {
        strcpy(port, "80");
    }

    /* Extract the path */
    pathbegin = strchr(hostbegin, '/');
    if (pathbegin == NULL)
    {
        pathname[0] = '\0';
    }
    else
    {
        // pathbegin++;
        strcpy(pathname, pathbegin);
    }

    return 0;
}

/*
 * format_log_entry - Create a formatted log entry in logstring.
 *
 * The inputs are the socket address of the requesting client
 * (sockaddr), the URI from the request (uri), the number of bytes
 * from the server (size).
 */
void format_log_entry(char *logstring, struct sockaddr_in *sockaddr,
                      char *uri, size_t size)
{
    time_t now;
    char time_str[MAXLINE];
    unsigned long host;
    unsigned char a, b, c, d;

    /* Get a formatted time string */
    now = time(NULL);
    strftime(time_str, MAXLINE, "%a %d %b %Y %H:%M:%S %Z", localtime(&now));

    /*
     * Convert the IP address in network byte order to dotted decimal
     * form. Note that we could have used inet_ntoa, but chose not to
     * because inet_ntoa is a Class 3 thread unsafe function that
     * returns a pointer to a static variable (Ch 12, CS:APP).
     */
    host = ntohl(sockaddr->sin_addr.s_addr);
    a = host >> 24;
    b = (host >> 16) & 0xff;
    c = (host >> 8) & 0xff;
    d = host & 0xff;

    /* Return the formatted log entry string */
    sprintf(logstring, "%s: %d.%d.%d.%d %s %zu", time_str, a, b, c, d, uri, size);
}

ssize_t Rio_readnb_w(rio_t *rp, void *usrbuf, size_t n)
{
    ssize_t rc;
    if ((rc = rio_readnb(rp, usrbuf, n)) < 0)
    {
        // P(&mutex);
        printf("Rio_readnb error\n");
        // V(&mutex);
    }
    return rc;
}

ssize_t Rio_readlineb_w(rio_t *rp, void *usrbuf, size_t maxlen)
{
    ssize_t rc;

    if ((rc = rio_readlineb(rp, usrbuf, maxlen)) < 0)
    {
        // P(&mutex);
        printf("Rio_readlineb error\n");
        // V(&mutex);
    }
    return rc;
}

ssize_t Rio_writen_w(int fd, void *usrbuf, size_t n)
{
    if (rio_writen(fd, usrbuf, n) != n)
    {
        // P(&mutex);
        printf("Rio_writen error\n");
        // V(&mutex);
    }
    return n;
}

ssize_t rio_read_w(rio_t *rp, char *usrbuf, size_t n)
{
    int cnt;

    while (rp->rio_cnt <= 0) {  /* Refill if buf is empty */
	rp->rio_cnt = read(rp->rio_fd, rp->rio_buf, 
			   sizeof(rp->rio_buf));
	if (rp->rio_cnt < 0) {
	    if (errno != EINTR) /* Interrupted by sig handler return */
		return -1;
	}
	else if (rp->rio_cnt == 0)  /* EOF */
	    return 0;
	else 
	    rp->rio_bufptr = rp->rio_buf; /* Reset buffer ptr */
    }

    /* Copy min(n, rp->rio_cnt) bytes from internal buf to user buf */
    cnt = n;          
    if (rp->rio_cnt < n)   
	cnt = rp->rio_cnt;
    memcpy(usrbuf, rp->rio_bufptr, cnt);
    rp->rio_bufptr += cnt;
    rp->rio_cnt -= cnt;
    return cnt;
}