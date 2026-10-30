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

// [BUF] Zeroes out the buffer. Good hygiene.
void lb_init(linebuf *lb)
{
    lb->len = 0;
    lb->buf[0] = '\0';
}

// [BUF] Safely appends new bytes to the buffer.
int lb_feed(linebuf *lb, const char *data, size_t n)
{
    if (lb->len + n > sizeof(lb->buf)) {
        /* Sender is not honouring the line-length rule (how rude . . . ). 
         * Drop what we hold and keep going rather than wedging the link. */
        lb->len = 0;
        return -1;
    }
    memcpy(lb->buf + lb->len, data, n);
    lb->len += n;
    return 0;
}

// [BUF] Extracts the next full message delimited by \n
int lb_next(linebuf *lb, char *out, size_t outsz)
{
    char  *nl;
    size_t linelen, rest;
    int    oversize = 0;

    // Search for a newline character in our current buffer
    nl = memchr(lb->buf, '\n', lb->len);
    
    if (nl == NULL) {
        if (lb->len >= OBDH_MAX_LINE) {
            /* No terminator within the legal 512-byte length. 
             * It's garbage. Cast it into the fires of Mount Doom */
            lb->len = 0;
            return -1;
        }
        return 0; // We have an incomplete line. Need to wait for more TCP packets.
    }

    // Math to figure out how long the line is, and how much is left over
    linelen = (size_t)(nl - lb->buf);
    rest    = lb->len - (linelen + 1);

    if (linelen >= outsz || linelen >= OBDH_MAX_LINE) {
        oversize = 1; // It's too big, reject it.
    } else {
        memcpy(out, lb->buf, linelen);
        out[linelen] = '\0'; // Null terminate so C strings don't explode
        
        /* Tolerate \r\n from telnet-style clients because we are benevolent */
        if (linelen > 0 && out[linelen - 1] == '\r')
            out[linelen - 1] = '\0';
    }

    // Shift the remaining unprocessed bytes to the front of the buffer. 
    // We use memmove, not memcpy, because the regions might overlap and we like living.
    memmove(lb->buf, nl + 1, rest);
    lb->len = rest;

    return oversize ? -1 : 1;
}

/* ------------------------------------------------------------------ */
/* Field splitting                                                     */
/* ------------------------------------------------------------------ */

// [STR] Mutates a string by replacing '|' with '\0' and saving the pointers
int split_fields(char *line, char *out[], int max)
{
    int   n = 0;
    char *p = line;

    if (max <= 0)
        return -1;

    out[n++] = p; // First field starts at the beginning
    while (*p) {
        if (*p == '|') {
            *p = '\0'; // Replace pipe with null terminator. Destructive but fast.
            if (n >= max)
                return -1; // Too many fields!
            out[n++] = p + 1; // Next field starts right after the null terminator
        }
        p++;
    }
    return n;
}

// [STR] Checks if someone is trying to send us weird binary garbage
int is_printable_ascii(const char *s)
{
    for (; *s; s++) {
        unsigned char c = (unsigned char)*s;
        if (c < 0x20 || c > 0x7E)
            return 0;
    }
    return 1;
}

/* (Remaining boilerplate socket networking and time functions omitted for brevity, 
 * as they are standard POSIX wrappers and less likely to cause junior devs pain) */