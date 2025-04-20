#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <signal.h>

#include "httpd.h"


int Stop = 0;

void sig_handler(int signo)
{
  if (signo == SIGINT) {
    Stop = 1;
  }

}

void handle_request(
    struct httpd_conn      *conn,
    const char             *method,
    const char             *url,
    struct httpd_header    *headers,
    int                     num_headers,
    const void             *request_data,
    int                     request_length,
    void                   *arg)
{
    httpd_respond(conn, 200, "text/plain", "here is some data", strlen("here is some data"));
}


int
 main(int argc, char *argv[])
{
    struct httpd *httpd;

    signal(SIGINT, sig_handler);

    httpd = httpd_start(8000, handle_request, NULL);

    while (!Stop) {
        usleep(100);
    }

    printf("stopping\n");

    httpd_stop(httpd);

    return 0;
}
