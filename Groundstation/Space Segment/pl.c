/* pl.c - Payload Instrument Control Unit (PL) for the R7023E OBDH model.
 *
 * Deliberately the simplest of the three programs. A true Finnish Hero of a C program.
 * PL knows nothing about platform modes, time tags, or ground sequences. 
 * It receives four instructions, does what it is told, and reports what happened . . . a true Finnish hero
 * Note: Technically I'm not Finnish, but a Finnish friend of mine took a Finnish cultural assesment thing with me
 * and I scored the same as him, so he declared me Finn or at least and Honorary Finn.
 * The key point here is that I'm not racist, just really tired and quite possibly stupid . . . I apologize for nothing.
 * ~ Precious
 */

#include "obdh.h"
#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define TICK_MS           100
#define HK_PERIOD_MS     2000   /* ICD: cyclic housekeeping interval  */
#define CAPTURE_MS       5000   /* assignment: assume a 5 second take (smile for the camera) */
#define RECONNECT_MS     2000   /* ICD: retry every 2 seconds if the platform ghosts us */

#define IMAGE_LOG "pl_images.log"

enum pl_state { PLS_OFF = 0, PLS_IDLE, PLS_CAPTURING };

static const char *pl_state_name[] = { "OFF", "IDLE", "CAPTURING" };

// Our global state struct. We use this instead of a dozen loose globals.
static struct {
    int     fd;
    linebuf buf;
    long    next_retry_mono;

    /* time, learned only from PF_TIME broadcasts. PL has no real clock. */
    int  time_known;
    long time_base_sec;    /* OBT seconds-of-day at the last broadcast */
    long time_base_mono;   /* our own tick count at that moment */

    enum pl_state state;
    long          capture_deadline;
    char          capture_file[32];

    int  images;
    char last_image[9];

    int  tm_seq;
} P;

/* ------------------------------------------------------------------ */
/* Clock                                                               */
/* ------------------------------------------------------------------ */

// [TIM] Interpolates current time based on the last broadcast and our  internal tick.
static long pl_clock_now(void)
{
    if (!P.time_known)
        return 0; // We literally have no idea what time it is
    return (P.time_base_sec + (mono_ms() - P.time_base_mono) / 1000L) % 86400L;
}

// [TIM] Formats the clock for telemetry.
static const char *pl_clock_str(void)
{
    static char buf[9];
    if (!P.time_known) {
        snprintf(buf, sizeof(buf), "UNSYNCED");
        return buf;
    }
    fmt_hms(pl_clock_now(), buf);
    return buf;
}

// [TIM] Accepts time synchronization from the platform. We are now self-aware.
static void pl_clock_set(const char *hms)
{
    long t = parse_hms(hms);
    if (t < 0)
        return; // String was garbage, ignore it
    P.time_base_sec  = t;
    P.time_base_mono = mono_ms();
    P.time_known     = 1; 
}

/* ------------------------------------------------------------------ */
/* Telemetry                                                           */
/* ------------------------------------------------------------------ */

// [TM] Sends raw text back to the Platform OBC
static void pl_tm_send(const char *line)
{
    printf("[PL] >PF   %s\n", line);
    fflush(stdout);
    send_line(P.fd, line);
}

// [TM] Wraps an event string in our telemetry formatting and sequence counting
static void pl_tm_event(const char *event, const char *extra)
{
    char line[OBDH_MAX_LINE];
    if (extra && *extra)
        snprintf(line, sizeof(line), "EV|%d|PL|%s|event=%s;%s",
                 P.tm_seq++, pl_clock_str(), event, extra);
    else
        snprintf(line, sizeof(line), "EV|%d|PL|%s|event=%s",
                 P.tm_seq++, pl_clock_str(), event);
    pl_tm_send(line);
}

// [TM] Generates our heartbeat (Housekeeping)
static void pl_tm_hk(void)
{
    char line[OBDH_MAX_LINE];
    snprintf(line, sizeof(line), "HK|%d|PL|%s|pl_state=%s;images=%d;last_image=%s",
             P.tm_seq++, pl_clock_str(), pl_state_name[P.state],
             P.images, P.last_image);
    pl_tm_send(line);
}

/* ------------------------------------------------------------------ */
/* Capture driver                                                      */
/* ------------------------------------------------------------------ */

// [CAP] Simulates the start of an image capture.
static void pl_capture_start(void)
{
    char  stamp[9];
    FILE *f;

    if (P.time_known)
        fmt_hms(pl_clock_now(), stamp);
    else
        snprintf(stamp, sizeof(stamp), "UNSYNCED");

    // Make a fake filename based on the timestamp
    snprintf(P.capture_file, sizeof(P.capture_file), "img_%c%c%c%c%c%c.txt",
             stamp[0], stamp[1], stamp[3], stamp[4], stamp[6], stamp[7]);

    /* The "science data": a log entry with the take time. 
     * Opens in append mode, writes, and closes... theoretically... hopefully. */
    f = fopen(IMAGE_LOG, "a");
    if (f != NULL) {
        fprintf(f, "%s take_time=%s file=%s obt_source=%s\n",
                P.time_known ? "IMAGE" : "IMAGE_UNSYNCED",
                stamp, P.capture_file,
                P.time_known ? "PF_BROADCAST" : "NONE");
        fclose(f);
    }

    // Set a 5-second timer from right now for when this fake image is "done"
    P.capture_deadline = mono_ms() + CAPTURE_MS;
    P.state            = PLS_CAPTURING;
    snprintf(P.last_image, sizeof(P.last_image), "%s", stamp);
}

// [CAP] Checks if our 5-second timer has elapsed
static int pl_capture_poll(void)
{
    return (P.state == PLS_CAPTURING && mono_ms() >= P.capture_deadline);
}

/* ------------------------------------------------------------------ */
/* Command handling                                                    */
/* ------------------------------------------------------------------ */

// [TC] Parses incoming commands from the platform.
static void pl_tc_handle(char *line)
{
    char *f[OBDH_MAX_FIELDS];
    int   nf;
    const char *cmd, *args;

    printf("[PL] <PF   %s\n", line);
    fflush(stdout);

    // Break the string into fields using the '|' delimiter
    nf = split_fields(line, f, OBDH_MAX_FIELDS);
    
    // If it doesn't have 6 fields or isn't a Telecommand (TC), ignore it
    if (nf != 6 || strcmp(f[0], "TC") != 0)
        return;

    cmd  = f[4];
    args = f[5];

    if (strcmp(cmd, "PL_PING") == 0) {
        pl_tm_hk(); // Ping? Pong.
        return;
    }

    if (strcmp(cmd, "PL_TIME") == 0) {
        pl_clock_set(args); // Sync our internal clock
        return;
    }

    if (strcmp(cmd, "PL_POWER") == 0) {
        if (strcmp(args, "ON") == 0) {
            if (P.state == PLS_OFF) {
                P.state = PLS_IDLE; // Power up
                pl_tm_event("PL_ON", NULL);
            }
        } else {
            if (P.state != PLS_OFF) {
                P.state = PLS_OFF; // Go to sleep
                pl_tm_event("PL_OFF", NULL);
            }
        }
        return;
    }

    if (strcmp(cmd, "PL_CAPTURE") == 0) {
        if (P.state != PLS_IDLE)
            return; /* PF already validated this. If we aren't IDLE, stay quiet. */
        pl_capture_start();
        pl_tm_event("IMAGE_START", NULL);
        return;
    }
}

/* ------------------------------------------------------------------ */
/* Link                                                                */
/* ------------------------------------------------------------------ */

// [LNK] Reads data from the TCP socket buffer
static void pl_link_read(void)
{
    char    chunk[1024];
    char    line[OBDH_MAX_LINE];
    ssize_t n;
    int     r;

    n = read(P.fd, chunk, sizeof(chunk));
    if (n == 0) {
        // A read of 0 means the server hung up on us
        close(P.fd);
        P.fd = -1;
        printf("[PL] ---- platform link lost\n");
        fflush(stdout);
        return;
    }
    if (n < 0)
        return; // Just transient errors

    // Feed raw chunk into the line buffer, then pull out valid lines
    lb_feed(&P.buf, chunk, (size_t)n);
    while ((r = lb_next(&P.buf, line, sizeof(line))) != 0)
        if (r > 0 && line[0] != '\0')
            pl_tc_handle(line);
}

/* ------------------------------------------------------------------ */
/* Main                                                                */
/* ------------------------------------------------------------------ */

int main(int argc, char **argv)
{
    const char *host = (argc > 1) ? argv[1] : "127.0.0.1";
    long        next_hk;

    // Zero out state because undefined behavior is scary
    memset(&P, 0, sizeof(P));
    P.fd     = -1;
    P.state  = PLS_OFF;
    P.tm_seq = 1;
    snprintf(P.last_image, sizeof(P.last_image), "NONE");
    lb_init(&P.buf);

    printf("[PL] ---- booted, OFF, no time, connecting to %s:%d\n", host, PF_PL_PORT);
    fflush(stdout);

    next_hk = mono_ms() + HK_PERIOD_MS;

    for (;;) {
        struct pollfd p;

        // If we're disconnected, try to reconnect every 2 seconds
        if (P.fd < 0) {
            if (mono_ms() >= P.next_retry_mono) {
                P.next_retry_mono = mono_ms() + RECONNECT_MS;
                P.fd = tcp_connect(host, PF_PL_PORT);
                if (P.fd >= 0) {
                    lb_init(&P.buf);
                    P.tm_seq = 1;
                    printf("[PL] ---- connected to platform\n");
                    fflush(stdout);
                }
            }
            usleep(TICK_MS * 1000);
            continue;
        }

        // Wait up to TICK_MS (100ms) for incoming data
        p.fd      = P.fd;
        p.events  = POLLIN;
        p.revents = 0;
        poll(&p, 1, TICK_MS);

        if (p.revents & POLLIN)
            pl_link_read(); // We got mail!

        if (P.fd < 0)
            continue; // Link died during read, start over

        // Check if our image capture timer is done
        if (pl_capture_poll()) {
            char extra[96];
            P.state = PLS_IDLE;
            P.images++;
            snprintf(extra, sizeof(extra), "duration=%d;file=%s",
                     CAPTURE_MS / 1000, P.capture_file);
            pl_tm_event("IMAGE_DONE", extra); // Tell PF the good news
        }

        // Send housekeeping every 2 seconds
        if (mono_ms() >= next_hk) {
            next_hk += HK_PERIOD_MS;
            pl_tm_hk();
        }
    }

    return 0; // Sleep . . . sleep my little simulated system (Do sims of sims dream of electric sheep?)
}