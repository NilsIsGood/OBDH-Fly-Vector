/* obdh.h - shared link and protocol helpers for the R7023E OBDH model.
 *
 * Everything in here is common to PF, PL and the ground test console.
 * It implements the transport and framing rules of ICD v0.1:
 *   - one message per line, terminated by '\n' (because TCP is a relentless stream of bytes)
 *   - fields separated by '|'
 *   - maximum 512 bytes per line, printable ASCII only
 *
 * Nothing in this file knows anything about telecommands, modes or
 * telemetry. That separation is deliberate so we don't create spaghetti code.
 */
#ifndef OBDH_H
#define OBDH_H

#include <stddef.h>

#define OBDH_MAX_LINE    512   /* ICD: maximum bytes on the wire, incl. terminator */
#define OBDH_MAX_FIELDS   8    /* ICD: longest message shape is 6 fields, giving us 2 spare just in case */

#define PF_GND_PORT     5001   /* ICD: PF listens here, Gnd connects in */
#define PF_PL_PORT      5002   /* ICD: PF listens here, PL connects in  */

/* ------------------------------------------------------------------ */
/* Line buffering                                                      */
/* ------------------------------------------------------------------ */
/* TCP is a byte stream. It does not care about things like message "boundaries", "feelings", or personal space . . .
 * Two messages can arrive in one read() and one message can arrive split across two. 
 * Everything received is fed into a linebuf, and complete lines are pulled out one at a time.
 */

typedef struct {
    char   buf[OBDH_MAX_LINE * 4]; // Oversized buffer because I have trust issues
    size_t len;
} linebuf;

void lb_init(linebuf *lb);

/* Append raw bytes. Returns 0 on success, -1 if the buffer is full
 * (which means a sender is ignoring the 512-byte line-length rule and we must judge them). */
int  lb_feed(linebuf *lb, const char *data, size_t n);

/* Pull out the next complete line, without its terminator.
 * Returns  1 : a line was written to out (yay)
 *          0 : no complete line available yet (keep waiting)
 *         -1 : an oversized or malformed line was discarded ("Nani?!") */
int  lb_next(linebuf *lb, char *out, size_t outsz);

/* ------------------------------------------------------------------ */
/* Field splitting                                                     */
/* ------------------------------------------------------------------ */
/* Splits line in place on '|'. The line is mutated, which is terrifying but efficient.
 * Returns the number of fields found, or -1 if there were more than max. */
int  split_fields(char *line, char *out[], int max);

/* True if every byte is printable ASCII (0x20..0x7E). No weird control characters allowed. */
int  is_printable_ascii(const char *s);

/* ------------------------------------------------------------------ */
/* Time                                                                */
/* ------------------------------------------------------------------ */
long mono_ms(void);                  /* monotonic milliseconds since boot (immune to time-traveling sysadmins) */
long wall_sec_of_day(void);          /* local wall clock as seconds since midnight */
void fmt_hms(long secs, char *out9); /* seconds-of-day -> "hh:mm:ss", out9 >= 9 bytes */
long parse_hms(const char *s);       /* "hh:mm:ss" -> seconds, or -1 if the string is garbage */

/* ------------------------------------------------------------------ */
/* Sockets                                                             */
/* ------------------------------------------------------------------ */
int  tcp_listen(int port);                      /* returns listening fd, or -1 on failure */
int  tcp_accept(int listen_fd);                 /* non-blocking accept, -1 if nobody is there */
int  tcp_connect(const char *host, int port);   /* returns connected fd, or -1 */
int  send_line(int fd, const char *line);       /* appends '\n'; -1 if fd<0 or write fails */

#endif /* OBDH_H */