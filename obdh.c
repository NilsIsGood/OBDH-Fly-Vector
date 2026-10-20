/* obdh.c - implementation of the shared link and protocol helpers. */

#include "obdh.h"

#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <stdio.h>
#include <string.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>

/* ------------------------------------------------------------------ */
/* Line buffering                                                      */
/* ------------------------------------------------------------------ */

void lb_init(linebuf *lb)
{
    lb->len = 0;
    lb->buf[0] = '\0';
}

int lb_feed(linebuf *lb, const char *data, size_t n)
{
    if (lb->len + n > sizeof(lb->buf)) {
        /* Sender is not honouring the line-length rule. Drop what we
         * hold and keep going rather than wedging the link.           */
        lb->len = 0;
        return -1;
    }
    memcpy(lb->buf + lb->len, data, n);
    lb->len += n;
    return 0;
}

int lb_next(linebuf *lb, char *out, size_t outsz)
{
    char  *nl;
    size_t linelen, rest;
    int    oversize = 0;

    nl = memchr(lb->buf, '\n', lb->len);
    if (nl == NULL) {
        if (lb->len >= OBDH_MAX_LINE) {
            /* No terminator within the legal length: discard. */
            lb->len = 0;
            return -1;
        }
        return 0;
    }

    linelen = (size_t)(nl - lb->buf);
    rest    = lb->len - (linelen + 1);

    if (linelen >= outsz || linelen >= OBDH_MAX_LINE) {
        oversize = 1;
    } else {
        memcpy(out, lb->buf, linelen);
        out[linelen] = '\0';
        /* Tolerate CRLF from a telnet-style client. */
        if (linelen > 0 && out[linelen - 1] == '\r')
            out[linelen - 1] = '\0';
    }

    memmove(lb->buf, nl + 1, rest);
    lb->len = rest;

    return oversize ? -1 : 1;
}

/* ------------------------------------------------------------------ */
/* Field splitting                                                     */
/* ------------------------------------------------------------------ */

int split_fields(char *line, char *out[], int max)
{
    int   n = 0;
    char *p = line;

    if (max <= 0)
        return -1;

    out[n++] = p;
    while (*p) {
        if (*p == '|') {
            *p = '\0';
            if (n >= max)
                return -1;
            out[n++] = p + 1;
        }
        p++;
    }
    return n;
}

int is_printable_ascii(const char *s)
{
    for (; *s; s++) {
        unsigned char c = (unsigned char)*s;
        if (c < 0x20 || c > 0x7E)
            return 0;
    }
    return 1;
}

/* ------------------------------------------------------------------ */
/* Time                                                                */
/* ------------------------------------------------------------------ */

long mono_ms(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (long)ts.tv_sec * 1000L + ts.tv_nsec / 1000000L;
}

long wall_sec_of_day(void)
{
    time_t     now = time(NULL);
    struct tm  tm_local;
    localtime_r(&now, &tm_local);
    return tm_local.tm_hour * 3600L + tm_local.tm_min * 60L + tm_local.tm_sec;
}

void fmt_hms(long secs, char *out9)
{
    long s = secs % 86400L;
    if (s < 0)
        s += 86400L;
    snprintf(out9, 9, "%02ld:%02ld:%02ld", s / 3600L, (s % 3600L) / 60L, s % 60L);
}

long parse_hms(const char *s)
{
    int h, m, sec;
    int i;

    if (s == NULL || strlen(s) != 8)
        return -1;
    if (s[2] != ':' || s[5] != ':')
        return -1;
    for (i = 0; i < 8; i++) {
        if (i == 2 || i == 5)
            continue;
        if (s[i] < '0' || s[i] > '9')
            return -1;
    }
    h   = (s[0] - '0') * 10 + (s[1] - '0');
    m   = (s[3] - '0') * 10 + (s[4] - '0');
    sec = (s[6] - '0') * 10 + (s[7] - '0');

    if (h > 23 || m > 59 || sec > 59)
        return -1;
    return h * 3600L + m * 60L + sec;
}

/* ------------------------------------------------------------------ */
/* Sockets                                                             */
/* ------------------------------------------------------------------ */

static void set_nonblocking(int fd)
{
    int flags = fcntl(fd, F_GETFL, 0);
    if (flags >= 0)
        fcntl(fd, F_SETFL, flags | O_NONBLOCK);
}

int tcp_listen(int port)
{
    int                fd, yes = 1;
    struct sockaddr_in addr;

    fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0)
        return -1;

    setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof(yes));

    memset(&addr, 0, sizeof(addr));
    addr.sin_family      = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_ANY);
    addr.sin_port        = htons((unsigned short)port);

    if (bind(fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        close(fd);
        return -1;
    }
    if (listen(fd, 4) < 0) {
        close(fd);
        return -1;
    }
    set_nonblocking(fd);
    return fd;
}

int tcp_accept(int listen_fd)
{
    int fd = accept(listen_fd, NULL, NULL);
    if (fd < 0)
        return -1;
    set_nonblocking(fd);
    return fd;
}

int tcp_connect(const char *host, int port)
{
    int                fd, yes = 1;
    struct sockaddr_in addr;

    fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0)
        return -1;

    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port   = htons((unsigned short)port);
    if (inet_pton(AF_INET, host, &addr.sin_addr) != 1) {
        close(fd);
        return -1;
    }

    if (connect(fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        close(fd);
        return -1;
    }

    setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &yes, sizeof(yes));
    set_nonblocking(fd);
    return fd;
}

int send_line(int fd, const char *line)
{
    char   out[OBDH_MAX_LINE];
    size_t n;
    ssize_t written;

    if (fd < 0)
        return -1;

    n = strlen(line);
    if (n + 2 > sizeof(out))
        return -1;

    memcpy(out, line, n);
    out[n++] = '\n';
    out[n]   = '\0';

    written = write(fd, out, n);
    if (written < 0)
        return (errno == EAGAIN || errno == EWOULDBLOCK) ? 0 : -1;
    return 0;
}
