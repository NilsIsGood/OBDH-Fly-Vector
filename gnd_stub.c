/* gnd_stub.c - minimal ground console for testing PF.
 *
 * THIS IS A TEST HARNESS, NOT THE GROUND SEGMENT DELIVERABLE.
 *
 * It exists so the space segment can be exercised before the real MCS
 * exists, and so the ground team has a working reference for the wire
 * format. It has no MIB file, no display logic and no error recovery
 * worth the name. Replace it.
 *
 * It does do the three things the ICD requires of a ground system:
 *   - maintains the ground telecommand sequence counter
 *   - stamps received telemetry with a ground reception time
 *   - appends everything received to a logfile
 *
 * Console syntax (translated into ICD telecommands):
 *   ping
 *   obt                 set OBT to this computer's wall clock
 *   obt 14:29:40        set OBT to a specific time
 *   mode TARGET         mode change, immediate
 *   mode SAFE @14:35:00 mode change, time tagged
 *   hk                  request one extra housekeeping frame
 *   power ON            payload on
 *   power OFF @14:40:00 payload off, time tagged
 *   image               take an image now
 *   image @14:32:10     take an image at a given time
 *   raw TC|9|PL|NOW|... send a literal line, for testing rejections
 *   quit
 *
 * Build: see Makefile.  Run: ./gnd_stub [host]
 */

#include "obdh.h"

#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define TM_LOG "gnd_tm.log"

static int   g_seq = 1;
static int   g_fd  = -1;
static FILE *g_log = NULL;

/* ------------------------------------------------------------------ */

static void gnd_send(const char *dest, const char *timetag,
                     const char *cmd, const char *args)
{
    char line[OBDH_MAX_LINE];
    snprintf(line, sizeof(line), "TC|%d|%s|%s|%s|%s",
             g_seq++, dest, timetag, cmd, args);
    printf("[GND] >PF   %s\n", line);
    fflush(stdout);
    send_line(g_fd, line);
}

static void gnd_receive(const char *line)
{
    char stamp[9];
    fmt_hms(wall_sec_of_day(), stamp);

    /* ICD: housekeeping is appended with a ground reception timestamp
     * and stored in a logfile after display.                          */
    printf("[GND] <PF   %s  (rx %s)\n", line, stamp);
    fflush(stdout);

    if (g_log != NULL) {
        fprintf(g_log, "%s %s\n", stamp, line);
        fflush(g_log);
    }
}

/* ------------------------------------------------------------------ */
/* Console input                                                       */
/* ------------------------------------------------------------------ */

/* Pulls an optional "@hh:mm:ss" out of the tail of a console line.
 * Returns the time tag, or "NOW".                                     */
static const char *extract_timetag(char *tail)
{
    static char tag[16];
    char *at = strchr(tail, '@');

    snprintf(tag, sizeof(tag), "NOW");
    if (at != NULL) {
        if (at[1] == '+') {
            /* "@+10" means ten seconds from now. Convenience for
             * scripted demonstrations, which cannot know the wall
             * clock in advance. Expanded here, on the ground: what
             * goes on the wire is always an absolute hh:mm:ss.      */
            long when = wall_sec_of_day() + atol(at + 2);
            char hms[9];
            fmt_hms(when, hms);
            snprintf(tag, sizeof(tag), "%s", hms);
        } else {
            snprintf(tag, sizeof(tag), "%s", at + 1);
        }
        *at = '\0';
        /* trim trailing whitespace left behind */
        while (at > tail && (at[-1] == ' ' || at[-1] == '\t'))
            *--at = '\0';
    }
    return tag;
}

static int gnd_console(char *line)
{
    char *verb, *rest;
    char  tag[16];

    while (*line == ' ')
        line++;
    if (*line == '\0' || *line == '#')
        return 0;

    verb = line;
    rest = strchr(line, ' ');
    if (rest != NULL) {
        *rest++ = '\0';
        while (*rest == ' ')
            rest++;
    } else {
        rest = line + strlen(line);
    }

    snprintf(tag, sizeof(tag), "%s", extract_timetag(rest));

    if (strcmp(verb, "quit") == 0 || strcmp(verb, "exit") == 0)
        return 1;

    if (strcmp(verb, "raw") == 0) {
        printf("[GND] >PF   %s\n", rest);
        fflush(stdout);
        send_line(g_fd, rest);
        return 0;
    }

    if (strcmp(verb, "ping") == 0) {
        gnd_send("PF", "NOW", "PING", "");
    } else if (strcmp(verb, "obt") == 0) {
        char now[9];
        if (*rest == '\0') {
            fmt_hms(wall_sec_of_day(), now);
            rest = now;
        }
        gnd_send("PF", "NOW", "SET_OBT", rest);
    } else if (strcmp(verb, "mode") == 0) {
        gnd_send("PF", tag, "SET_MODE", rest);
    } else if (strcmp(verb, "hk") == 0) {
        gnd_send("PF", "NOW", "GET_HK", "");
    } else if (strcmp(verb, "power") == 0) {
        gnd_send("PL", tag, "PL_POWER", rest);
    } else if (strcmp(verb, "image") == 0) {
        gnd_send("PL", tag, "TAKE_IMAGE", "");
    } else if (strcmp(verb, "typo") == 0) {
        /* A deliberately mistyped command, sent with a correct sequence
         * number so that the run continues normally afterwards.        */
        gnd_send("PF", "NOW", "TAKE_PICTURE", "");
    } else {
        printf("[GND] ??   unknown console verb: %s\n", verb);
        fflush(stdout);
    }
    return 0;
}

/* ------------------------------------------------------------------ */

int main(int argc, char **argv)
{
    const char *host = (argc > 1) ? argv[1] : "127.0.0.1";
    linebuf     lb, in_lb;
    char        stdin_line[OBDH_MAX_LINE];
    int         tries;

    lb_init(&lb);
    lb_init(&in_lb);

    for (tries = 0; tries < 10 && g_fd < 0; tries++) {
        g_fd = tcp_connect(host, PF_GND_PORT);
        if (g_fd < 0)
            usleep(500000);
    }
    if (g_fd < 0) {
        fprintf(stderr, "[GND] cannot reach platform at %s:%d\n", host, PF_GND_PORT);
        return 1;
    }

    g_log = fopen(TM_LOG, "a");

    printf("[GND] ---- connected to platform, logging telemetry to %s\n", TM_LOG);
    printf("[GND] ---- try: obt / mode TARGET / power ON / image @hh:mm:ss / quit\n");
    fflush(stdout);

    for (;;) {
        struct pollfd p[2];
        char          chunk[1024];
        char          line[OBDH_MAX_LINE];
        ssize_t       n;
        int           r;

        p[0].fd = g_fd;          p[0].events = POLLIN; p[0].revents = 0;
        p[1].fd = STDIN_FILENO;  p[1].events = POLLIN; p[1].revents = 0;

        poll(p, 2, 200);

        if (p[0].revents & POLLIN) {
            n = read(g_fd, chunk, sizeof(chunk));
            if (n <= 0) {
                printf("[GND] ---- platform link lost\n");
                fflush(stdout);
                break;
            }
            lb_feed(&lb, chunk, (size_t)n);
            while ((r = lb_next(&lb, line, sizeof(line))) != 0)
                if (r > 0 && line[0] != '\0')
                    gnd_receive(line);
        }

        if (p[1].revents & POLLIN) {
            /* Read stdin the same way as the socket. Using fgets() here
             * would pull several lines into the stdio buffer while
             * poll() only reports once, so console commands would be
             * processed one step behind the operator.                  */
            int done = 0;

            n = read(STDIN_FILENO, chunk, sizeof(chunk));
            if (n <= 0)
                break;
            lb_feed(&in_lb, chunk, (size_t)n);
            while ((r = lb_next(&in_lb, stdin_line, sizeof(stdin_line))) != 0) {
                if (r > 0 && gnd_console(stdin_line)) {
                    done = 1;
                    break;
                }
            }
            if (done)
                break;
        }
    }

    if (g_log != NULL)
        fclose(g_log);
    if (g_fd >= 0)
        close(g_fd);
    return 0;
}
