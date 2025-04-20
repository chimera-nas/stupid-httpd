#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <pthread.h>
#include <sys/epoll.h>
#include <sys/eventfd.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>

#include "httpd.h"

#define HTTPD_MAX_REQUEST (1024*1024)
#define HTTPD_MAX_HEADERS 64

#define list_append2(head, add, prev, next)           \
    do {                                              \
        if (head) {                                   \
            (add)->prev        = (head)->prev;        \
            (head)->prev->next = (add);               \
            (head)->prev       = (add);               \
            (add)->next        = NULL;                \
        } else {                                      \
            (head)       = (add);                     \
            (head)->prev = (head);                    \
            (head)->next = NULL;                      \
        }                                             \
    } while (0)

#define list_delete2(head, del, prev, next)           \
    do {                                              \
        if ((del)->prev == (del)) {                   \
            (head) = NULL;                            \
        } else if ((del) == (head)) {                 \
            (del)->next->prev = (del)->prev;          \
            (head)            = (del)->next;          \
        } else {                                      \
            (del)->prev->next = (del)->next;          \
            if ((del)->next) {                        \
                (del)->next->prev = (del)->prev;      \
            } else {                                  \
                (head)->prev = (del)->prev;           \
            }                                         \
        }                                             \
    } while (0)

#define list_append(head, add)            list_append2(head, add, prev, next)
#define list_delete(head, del)            list_delete2(head, del, prev, next)

struct httpd_conn {
    int                 fd;
    socklen_t           sa_size;
    int                 length;
    int                 content_length;
    int                 expected;
    int                 num_headers;
    uint8_t            *buffer;
    const char         *method;
    const char         *url;
    struct httpd_header headers[HTTPD_MAX_HEADERS];
    struct sockaddr_in   sa;
    struct httpd       *httpd;
    struct httpd_conn  *prev;
    struct httpd_conn  *next;
};

struct httpd {
    int         epoll_fd;
    int         event_fd;
    int         listen_fd;
    int         run;
    struct httpd_conn *conns;
    pthread_t   thread;
    httpd_callback  callback;
    void           *callback_arg;
};

void
close_conn(struct httpd_conn *conn) 
{
    struct httpd *httpd = conn->httpd;

    list_delete(httpd->conns, conn);
    close(conn->fd);
    free(conn->buffer);
    free(conn);

}

int
parse_headers(struct httpd_conn *conn)
{
    char *hdrtext = (char*)conn->buffer;
    char *line, *nl, *spc, *spc2, *colon, *version, *term;
    struct httpd_header *hdr;
    int i, hdrlen;

    term = strstr(hdrtext, "\r\n\r\n");

    if (term == NULL) {
        /* we don't have the full headers yet, wait */
        return -1;
    }

    hdrlen = term - hdrtext + 4;

    /*
     * At this point we know we either have full legit headers
     * or we're going to detect a problem and kill the connection
     * before exiting, so we can munge strings in place
     */

    nl = index(hdrtext, '\r');

    line = hdrtext;
    *nl = '\0';

    spc = index(line, ' ');

    if (spc == NULL) {
        close_conn(conn);
        return -1;
    }

    conn->method = line;
    *spc = '\0';

    spc2 = index(spc+1, ' ');

    if (spc == NULL) {
        close_conn(conn);
        return -1;
    }

    conn->url = spc+1;
    *spc2 = '\0';

    version = spc2+1;

    if (strcmp(version,"HTTP/1.1") != 0 &&
        strcmp(version,"HTTP/1.0") != 0) {
        close_conn(conn);
        return -1;
    }

    if (*(nl+1) != '\n') {
        close_conn(conn);
        return -1;
    }

    line = nl+2;

    while (1) {

        nl = index(line, '\r');

        if (!nl) {
            close_conn(conn);
            return -1;
        }

        *nl = '\0';

        if (*(nl+1) != '\n') {
            close_conn(conn);
            return -1;
        }

        if (strlen(line) > 0) {

            if (conn->num_headers == HTTPD_MAX_HEADERS) {
                close_conn(conn);
                return -1;
            }

            colon = index(line, ':');

            if (!colon) {
                close_conn(conn);
                return -1;
            }

            *colon = '\0';

            spc = colon + 1;
            while (*spc == ' ') spc++;

            conn->headers[conn->num_headers].key = line;
            conn->headers[conn->num_headers].value = spc;
            conn->num_headers++;

            line = nl + 2;
        } else {
            break;
        }
    }

    conn->content_length = 0;
    conn->expected = hdrlen;

    for (i = 0; i < conn->num_headers; ++i) {
        hdr = &conn->headers[i];

        if (strcasecmp(hdr->key,"content-length") == 0) {
            conn->content_length = atoi(hdr->value);

            if (conn->content_length < 0) {
                close_conn(conn);
                return -1;
            }

            conn->expected = hdrlen + conn->content_length;

            if (conn->expected > HTTPD_MAX_REQUEST) {
                close_conn(conn);
                return -1;
            }
            
        }
    }

    return 0;
}

void
httpd_respond(struct httpd_conn *conn, int ok, const char *content_type, const void *data, int len)
{
    char hdrtext[256], *hdrp = hdrtext;;
    int hdrlen;
    ssize_t written, res;

    if (ok) {
        hdrp += sprintf(hdrp, "HTTP/1.1 200 OK\r\n");
    } else {
        hdrp += sprintf(hdrp, "HTTP/1.1 400 Bad Request\r\n");
    }

    hdrp += sprintf(hdrp, "Content-Length: %d\r\n", len);
    hdrp += sprintf(hdrp, "Connection: close\r\n");
    
    if (content_type) {
        hdrp += sprintf(hdrp, "Content-Type: %s\r\n", content_type);
    }

    *hdrp++ = '\r';
    *hdrp++ = '\n';
    *hdrp = '\0';

    hdrlen = hdrp - hdrtext;

    written = 0;

    while (written < hdrlen) {

        res = write(conn->fd, hdrtext + written, hdrlen - written);

        if (res < 0) {
            return;
        }

        written += res;
    }

    written = 0;
        
    while (written < len) {

        res = write(conn->fd, data + written, len - written);

        if (res < 0) {
            return;
        }

        written += res;
    }
}

void *
httpd_thread(void *arg)
{
    struct httpd *httpd = arg;
    struct httpd_conn *conn;
    int n, rc;
    struct epoll_event ev;
    ssize_t len;

    while (httpd->run) {
        n = epoll_wait(httpd->epoll_fd, &ev, 1, 1000);

        if (n <= 0) continue;

        if (ev.data.ptr == NULL) {
            /* event_fd, do nothing */
        } else if (ev.data.ptr == httpd) {

            conn = calloc(1, sizeof(*conn));

            conn->httpd = httpd;

            conn->fd = accept(httpd->listen_fd, (struct sockaddr *)&conn->sa, &conn->sa_size);

            if (conn->fd < 0) {
                free(conn);
                continue;
            }

            ev.events = EPOLLIN | EPOLLERR;
            ev.data.ptr = conn;

            rc = epoll_ctl(httpd->epoll_fd, EPOLL_CTL_ADD, conn->fd, &ev);

            conn->buffer = malloc(HTTPD_MAX_REQUEST);

            conn->expected = -1;
            list_append(httpd->conns, conn);

        } else {

            conn = ev.data.ptr;

            len = read(conn->fd, conn->buffer + conn->length, HTTPD_MAX_REQUEST - conn->length);

            if (len <= 0) {
                close_conn(conn);
                continue;
            } 

            conn->length += len;

            conn->buffer[conn->length] = 0;

            if (conn->expected < 0) {
                rc = parse_headers(conn);
                if (rc) continue;
            }

            if (conn->length >= conn->expected) {
                httpd->callback(
                    conn,
                    conn->method,
                    conn->url,
                    conn->headers,
                    conn->num_headers,
                    conn->buffer,
                    conn->content_length,
                    httpd->callback_arg);
    
                close_conn(conn);
            }


        }
    }

    return NULL;
}


struct httpd *
httpd_start(int port, httpd_callback callback, void *arg)
{
    struct httpd *httpd;
    int rc, on = 1;
    struct sockaddr_in sa;
    struct epoll_event ev;

    httpd = calloc(1, sizeof(*httpd));

    httpd->callback         = callback;
    httpd->callback_arg     = arg;

    if (!httpd) {
        goto err;
    }

    httpd->epoll_fd = epoll_create(64);

    if (httpd->epoll_fd < 0) {
        goto err;
    }

    httpd->event_fd = eventfd(0, 0);

    if (httpd->event_fd < 0) {
        goto err;
    }

    ev.events = EPOLLIN | EPOLLERR;
    ev.data.ptr = NULL;

    rc = epoll_ctl(httpd->epoll_fd, EPOLL_CTL_ADD, httpd->event_fd, &ev);

    if (rc) {
        goto err;
    }

    httpd->listen_fd = socket(PF_INET,SOCK_STREAM,0);

    if (httpd->listen_fd < 0) {
        goto err;
    }

    rc = setsockopt(httpd->listen_fd, SOL_SOCKET,SO_REUSEADDR,&on,sizeof(on));

    if (rc) {
        goto err;
    }

    memset(&sa,0,sizeof(sa));
    sa.sin_family = AF_INET;
    sa.sin_addr.s_addr = htonl(INADDR_ANY);
    sa.sin_port = htons(port);

    rc = bind(httpd->listen_fd,(struct sockaddr *)&sa,sizeof(sa));

    if (rc) {
        goto err;
    }

    rc = listen(httpd->listen_fd, 64);

    if (rc) {
        goto err;
    }

    ev.events = EPOLLIN | EPOLLERR;
    ev.data.ptr = httpd;
   
    rc = epoll_ctl(httpd->epoll_fd, EPOLL_CTL_ADD, httpd->listen_fd, &ev);

    if (rc) {
        goto err;
    }

    httpd->run = 1;

    pthread_create(&httpd->thread, NULL, httpd_thread, httpd);
    return httpd;

err:

    if (httpd) {
        if (httpd->listen_fd) close(httpd->listen_fd);
        if (httpd->event_fd) close(httpd->event_fd);
        if (httpd->epoll_fd) close(httpd->epoll_fd);
        free(httpd);
    }

    return NULL;

}

void
httpd_stop(struct httpd *httpd)
{
    uint64_t word = 1;

    httpd->run = 0;

    (void)!write(httpd->event_fd, &word, sizeof(word));

    pthread_join(httpd->thread, NULL);

    while (httpd->conns) {
        close_conn(httpd->conns);
    }

    close(httpd->listen_fd);
    close(httpd->event_fd);
    close(httpd->epoll_fd);
    free(httpd);
}
