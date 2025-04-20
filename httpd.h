#ifndef __HTTPD_H__
#define __HTTPD_H__

struct httpd;
struct httpd_conn;

struct httpd_header {
    const char         *key;
    const char         *value;
};

typedef void (*httpd_callback)(
    struct httpd_conn      *conn,
    const char             *method,
    const char             *url,
    struct httpd_header    *headers,
    int                     num_headers,
    const void             *request_data,
    int                     request_length,
    void                   *arg);


struct httpd *
httpd_start(int port, httpd_callback callback, void *arg);

void
httpd_stop(struct httpd *httpd);

void
httpd_respond(struct httpd_conn *conn, int ok, const char *content_type, const void *data, int len);

#endif
