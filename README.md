# stupid-httpd

stupid-httpd is a very simple minded HTTP server for embedding into C programs.

It runs in a single thread, handles only HTTP/1.1, uses blocking socket
operations, and only supports responses that are 200 (OK) or 400 (Bad Request).

Heavy emphasis on keeping it simple so the scope of bugs it can have is small.

You probably don't want to use it, but it can be suitable for use cases such
as serving prometheus metrics from something like this:

https://github.com/chimera-nas/prometheus-c.

It has no support for SSL nor any form of authentication.

There is a more performance-oriented http server in progress as part of libevpl:

https://github.com/chimera-nas/libevpl
