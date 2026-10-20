/* pf.c - Platform Onboard Computer (PF) for the R7023E OBDH model.
 *
 * Implements ICD v0.1. PF is the hub: it listens on two TCP ports,
 * validates everything arriving from the ground, schedules time-tagged
 * commands, and relays simplified instructions to the payload.
 *
 * Structure (mirrors the Space Segment function tree):
 *   pf_main            - 100 ms tick loop
 *   pf_link_*          - accept, read, dispatch           (link layer)
 *   pf_clock_*         - SCET / OBT                       (logic layer)
 *   pf_mode_*          - SAFE / TRANSITION / TARGET
 *   pf_tc_validate     - the ten checks, in ICD order
 *   pf_tc_execute      - dispatch of an accepted command
 *   pf_queue_*         - time-tag queue
 *   pf_tm_*            - ACK / NAK / HK / EV generation
 *
 * Build: see Makefile.  Run: ./pf
 */

#include "obdh.h"

#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define TICK_MS              100   /* main loop period                        */
#define HK_PERIOD_MS        2000   /* ICD: cyclic housekeeping interval       */
#define MODE_TRANSITION_MS  8000   /* ICD: mode changes take time             */
#define QUEUE_SIZE            16   /* ICD: time-tag queue capacity            */

/* ------------------------------------------------------------------ */
/* State                                                               */
/* ------------------------------------------------------------------ */

enum mode { M_SAFE = 0, M_TRANSITION, M_TARGET };

static const char *mode_name[] = { "SAFE", "TRANSITION", "TARGET" };

/* PF mirrors the payload state purely so that it can reject TAKE_IMAGE
 * with PL_BUSY. It is not reported in PF housekeeping; the ground sees
 * the payload's own forwarded housekeeping for that.                  */
enum pl_state { PLS_OFF = 0, PLS_IDLE, PLS_CAPTURING };

typedef struct {
    int  used;
    long due;              /* seconds of day, OBT                       */
    int  ref_seq;          /* ground sequence number that created it    */
    char dest[4];
    char cmd[32];
    char args[64];
} queue_entry;

static struct {
    /* links */
    int     gnd_listen, pl_listen;
    int     gnd_fd, pl_fd;
    linebuf gnd_buf, pl_buf;

    /* clock */
    long boot_mono;
    int  obt_valid;
    long obt_offset;       /* OBT seconds-of-day = SCET + offset        */

    /* modes */
    enum mode     mode;
    enum mode     mode_target;
    long          mode_deadline;   /* mono_ms when the transition ends  */
    int           pend_mode_ref;   /* ground seq awaiting EXECUTED ack  */

    /* payload mirror */
    int           pl_power;        /* 1 = ON                            */
    enum pl_state pl_state;
    int           pend_power_ref;
    int           pend_image_ref;

    /* sequence counters */
    int  tm_seq;           /* PF -> Gnd                                 */
    int  pl_tc_seq;        /* PF -> PL                                  */
    int  last_gnd_seq;     /* last TC seq seen from Gnd                 */
    int  last_pl_seq;      /* last TM seq seen from PL                  */

    /* scheduling */
    queue_entry queue[QUEUE_SIZE];
    long        next_hk_mono;
} S;

/* ------------------------------------------------------------------ */
/* Clock                                                               */
/* ------------------------------------------------------------------ */

static long pf_clock_scet(void)
{
    return (mono_ms() - S.boot_mono) / 1000L;
}

static long pf_clock_now(void)
{
    long t = pf_clock_scet();
    if (S.obt_valid)
        t = (t + S.obt_offset) % 86400L;
    return t;
}

static const char *pf_clock_str(void)
{
    static char buf[9];
    fmt_hms(pf_clock_now(), buf);
    return buf;
}

/* ------------------------------------------------------------------ */
/* Telemetry generation                                                */
/* ------------------------------------------------------------------ */

static void pf_tm_send(const char *line)
{
    printf("[PF] >GND  %s\n", line);
    fflush(stdout);
    send_line(S.gnd_fd, line);
}

static void pf_tm_ack(int ref, const char *stage, const char *cmd)
{
    char line[OBDH_MAX_LINE];
    snprintf(line, sizeof(line), "ACK|%d|PF|%s|ref=%d;stage=%s;cmd=%s",
             S.tm_seq++, pf_clock_str(), ref, stage, cmd);
    pf_tm_send(line);
}

static void pf_tm_nak(int ref, const char *reason, const char *detail)
{
    char line[OBDH_MAX_LINE];
    snprintf(line, sizeof(line), "NAK|%d|PF|%s|ref=%d;reason=%s;detail=%s",
             S.tm_seq++, pf_clock_str(), ref, reason, detail);
    pf_tm_send(line);
}

static void pf_tm_event(const char *event, const char *extra)
{
    char line[OBDH_MAX_LINE];
    if (extra && *extra)
        snprintf(line, sizeof(line), "EV|%d|PF|%s|event=%s;%s",
                 S.tm_seq++, pf_clock_str(), event, extra);
    else
        snprintf(line, sizeof(line), "EV|%d|PF|%s|event=%s",
                 S.tm_seq++, pf_clock_str(), event);
    pf_tm_send(line);
}

static int pf_queue_count(void)
{
    int i, n = 0;
    for (i = 0; i < QUEUE_SIZE; i++)
        if (S.queue[i].used)
            n++;
    return n;
}

static void pf_tm_hk(void)
{
    char line[OBDH_MAX_LINE];
    snprintf(line, sizeof(line),
             "HK|%d|PF|%s|mode=%s;obt_valid=%d;pl_power=%s;pl_link=%s;queued=%d;uptime=%ld",
             S.tm_seq++, pf_clock_str(), mode_name[S.mode], S.obt_valid,
             S.pl_power ? "ON" : "OFF", S.pl_fd >= 0 ? "UP" : "DOWN",
             pf_queue_count(), pf_clock_scet());
    pf_tm_send(line);
}

/* ------------------------------------------------------------------ */
/* Payload link                                                        */
/* ------------------------------------------------------------------ */

static void pf_pl_send(const char *cmd, const char *args)
{
    char line[OBDH_MAX_LINE];
    if (S.pl_fd < 0)
        return;
    snprintf(line, sizeof(line), "TC|%d|PL|NOW|%s|%s", S.pl_tc_seq++, cmd, args);
    printf("[PF] >PL   %s\n", line);
    fflush(stdout);
    send_line(S.pl_fd, line);
}

static void pf_pl_broadcast_time(void)
{
    char hms[9];
    if (S.pl_fd < 0)
        return;
    fmt_hms(pf_clock_now(), hms);
    pf_pl_send("PL_TIME", hms);
}

/* ------------------------------------------------------------------ */
/* Modes                                                               */
/* ------------------------------------------------------------------ */

static void pf_mode_start(enum mode target, int ref)
{
    char extra[96];

    snprintf(extra, sizeof(extra), "from=%s;to=%s",
             mode_name[S.mode], mode_name[target]);
    pf_tm_event("MODE_START", extra);

    /* ICD: entering SAFE implies an automated payload shutdown, and it
     * happens at the start of the transition, not at the end.          */
    if (target == M_SAFE && S.pl_power) {
        pf_pl_send("PL_POWER", "OFF");
        pf_tm_event("PL_AUTO_OFF", "cause=SAFE_ENTRY");
    }

    S.mode_target   = target;
    S.mode          = M_TRANSITION;
    S.mode_deadline = mono_ms() + MODE_TRANSITION_MS;
    S.pend_mode_ref = ref;
}

static void pf_mode_service(void)
{
    char extra[48];

    if (S.mode != M_TRANSITION)
        return;
    if (mono_ms() < S.mode_deadline)
        return;

    S.mode = S.mode_target;
    snprintf(extra, sizeof(extra), "mode=%s", mode_name[S.mode]);
    pf_tm_event("MODE_DONE", extra);

    if (S.pend_mode_ref >= 0) {
        pf_tm_ack(S.pend_mode_ref, "EXECUTED", "SET_MODE");
        S.pend_mode_ref = -1;
    }
}

/* ------------------------------------------------------------------ */
/* Command catalogue                                                   */
/* ------------------------------------------------------------------ */

typedef struct {
    const char *name;
    const char *dest;     /* "PF" or "PL"                              */
    int         takes_arg;
    int         allows_timetag;
} catalogue_entry;

static const catalogue_entry catalogue[] = {
    { "PING",       "PF", 0, 0 },
    { "SET_OBT",    "PF", 1, 0 },
    { "SET_MODE",   "PF", 1, 1 },
    { "GET_HK",     "PF", 0, 0 },
    { "PL_POWER",   "PL", 1, 1 },
    { "TAKE_IMAGE", "PL", 0, 1 },
    { NULL,         NULL, 0, 0 }
};

static const catalogue_entry *pf_catalogue_find(const char *name)
{
    int i;
    for (i = 0; catalogue[i].name != NULL; i++)
        if (strcmp(catalogue[i].name, name) == 0)
            return &catalogue[i];
    return NULL;
}

/* ------------------------------------------------------------------ */
/* Preconditions (ICD checks 8 and 9)                                  */
/*                                                                     */
/* Evaluated twice: once at uplink for the acceptance decision, and    */
/* again at execution time before the command is dispatched.           */
/* ------------------------------------------------------------------ */

static const char *pf_preconditions(const char *cmd, const char *args)
{
    if (strcmp(cmd, "SET_MODE") == 0) {
        if (S.mode == M_TRANSITION)
            return "IN_TRANSITION";
        return NULL;
    }
    if (strcmp(cmd, "PL_POWER") == 0) {
        if (S.pl_fd < 0)
            return "PL_LINK_DOWN";
        if (strcmp(args, "ON") == 0 && S.mode != M_TARGET)
            return "WRONG_MODE";
        return NULL;
    }
    if (strcmp(cmd, "TAKE_IMAGE") == 0) {
        if (S.mode != M_TARGET)
            return "WRONG_MODE";
        if (S.pl_fd < 0)
            return "PL_LINK_DOWN";
        if (!S.pl_power)
            return "PL_OFF";
        if (S.pl_state == PLS_CAPTURING)
            return "PL_BUSY";
        return NULL;
    }
    return NULL;
}

/* ------------------------------------------------------------------ */
/* Execution                                                           */
/* ------------------------------------------------------------------ */

static void pf_tc_execute(int ref, const char *cmd, const char *args)
{
    if (strcmp(cmd, "PING") == 0) {
        pf_tm_ack(ref, "EXECUTED", "PING");
        return;
    }

    if (strcmp(cmd, "SET_OBT") == 0) {
        char old[9], neu[9];
        long want = parse_hms(args);
        fmt_hms(pf_clock_now(), old);
        S.obt_offset = want - pf_clock_scet();
        S.obt_valid  = 1;
        fmt_hms(pf_clock_now(), neu);
        {
            char extra[64];
            snprintf(extra, sizeof(extra), "from=%s;to=%s", old, neu);
            pf_tm_event("OBT_SET", extra);
        }
        pf_pl_broadcast_time();          /* ICD: broadcast on OBT set */
        pf_tm_ack(ref, "EXECUTED", "SET_OBT");
        return;
    }

    if (strcmp(cmd, "SET_MODE") == 0) {
        enum mode target = (strcmp(args, "SAFE") == 0) ? M_SAFE : M_TARGET;
        pf_mode_start(target, ref);
        return;
    }

    if (strcmp(cmd, "GET_HK") == 0) {
        pf_tm_hk();
        pf_tm_ack(ref, "EXECUTED", "GET_HK");
        return;
    }

    if (strcmp(cmd, "PL_POWER") == 0) {
        pf_pl_send("PL_POWER", args);
        S.pend_power_ref = ref;          /* ack EXECUTED when PL confirms */
        return;
    }

    if (strcmp(cmd, "TAKE_IMAGE") == 0) {
        pf_pl_send("PL_CAPTURE", "");
        S.pend_image_ref = ref;
        return;
    }
}

/* ------------------------------------------------------------------ */
/* Time-tag queue                                                      */
/* ------------------------------------------------------------------ */

static int pf_queue_add(long due, int ref, const char *dest,
                        const char *cmd, const char *args)
{
    int i;
    for (i = 0; i < QUEUE_SIZE; i++) {
        if (!S.queue[i].used) {
            S.queue[i].used    = 1;
            S.queue[i].due     = due;
            S.queue[i].ref_seq = ref;
            snprintf(S.queue[i].dest, sizeof(S.queue[i].dest), "%s", dest);
            snprintf(S.queue[i].cmd,  sizeof(S.queue[i].cmd),  "%s", cmd);
            snprintf(S.queue[i].args, sizeof(S.queue[i].args), "%s", args);
            return 0;
        }
    }
    return -1;
}

static void pf_queue_service(void)
{
    long now = pf_clock_now();
    int  i;

    if (!S.obt_valid)
        return;

    for (i = 0; i < QUEUE_SIZE; i++) {
        queue_entry *q = &S.queue[i];
        const char  *why;
        char         extra[128];

        if (!q->used || q->due > now)
            continue;

        q->used = 0;

        snprintf(extra, sizeof(extra), "ref=%d;cmd=%s", q->ref_seq, q->cmd);
        pf_tm_event("TC_DUE", extra);

        /* ICD rule 3: re-check at execution time. A failure here is an
         * event, not a NAK, because acceptance was already reported.   */
        why = pf_preconditions(q->cmd, q->args);
        if (why != NULL) {
            snprintf(extra, sizeof(extra), "ref=%d;reason=%s", q->ref_seq, why);
            pf_tm_event("TC_FAILED", extra);
            continue;
        }

        pf_tc_execute(q->ref_seq, q->cmd, q->args);
    }
}

/* ------------------------------------------------------------------ */
/* Validation (ICD checks 1 to 10, in order)                           */
/* ------------------------------------------------------------------ */

static void pf_tc_handle(char *line)
{
    char *f[OBDH_MAX_FIELDS];
    int   nf, seq;
    const catalogue_entry *cat;
    const char *why;
    long  due = -1;
    int   future = 0;

    printf("[PF] <GND  %s\n", line);
    fflush(stdout);

    /* 1. format ---------------------------------------------------- */
    if (!is_printable_ascii(line)) {
        pf_tm_nak(0, "BAD_FORMAT", "non printable characters");
        return;
    }
    nf = split_fields(line, f, OBDH_MAX_FIELDS);
    if (nf != 6) {
        pf_tm_nak(0, "BAD_FORMAT", "expected 6 fields");
        return;
    }
    if (strcmp(f[0], "TC") != 0) {
        pf_tm_nak(0, "BAD_FORMAT", "not a telecommand");
        return;
    }

    /* 2. sequence -------------------------------------------------- */
    seq = atoi(f[1]);
    if (seq <= 0) {
        pf_tm_nak(0, "SEQ_ERROR", "sequence not parseable");
        return;
    }
    if (seq <= S.last_gnd_seq) {
        pf_tm_nak(seq, "SEQ_ERROR", "duplicate or out of order");
        return;
    }
    if (S.last_gnd_seq > 0 && seq > S.last_gnd_seq + 1) {
        char extra[64];
        snprintf(extra, sizeof(extra), "expected=%d;got=%d", S.last_gnd_seq + 1, seq);
        pf_tm_event("SEQ_GAP", extra);   /* reported, not rejected */
    }
    S.last_gnd_seq = seq;

    /* 3. catalogue ------------------------------------------------- */
    cat = pf_catalogue_find(f[4]);
    if (cat == NULL) {
        pf_tm_nak(seq, "UNKNOWN_CMD", "not in catalogue");
        return;
    }

    /* 4. arguments ------------------------------------------------- */
    if (strcmp(f[2], cat->dest) != 0) {
        pf_tm_nak(seq, "BAD_ARG", "destination does not match command");
        return;
    }
    if (cat->takes_arg && f[5][0] == '\0') {
        pf_tm_nak(seq, "BAD_ARG", "argument required");
        return;
    }
    if (!cat->takes_arg && f[5][0] != '\0') {
        pf_tm_nak(seq, "BAD_ARG", "command takes no argument");
        return;
    }
    if (strcmp(cat->name, "SET_MODE") == 0 &&
        strcmp(f[5], "SAFE") != 0 && strcmp(f[5], "TARGET") != 0) {
        pf_tm_nak(seq, "BAD_ARG", "mode must be SAFE or TARGET");
        return;
    }
    if (strcmp(cat->name, "PL_POWER") == 0 &&
        strcmp(f[5], "ON") != 0 && strcmp(f[5], "OFF") != 0) {
        pf_tm_nak(seq, "BAD_ARG", "power must be ON or OFF");
        return;
    }
    if (strcmp(cat->name, "SET_OBT") == 0 && parse_hms(f[5]) < 0) {
        pf_tm_nak(seq, "BAD_ARG", "time must be hh:mm:ss");
        return;
    }

    /* 5, 6, 7. time tag -------------------------------------------- */
    if (strcmp(f[3], "NOW") != 0) {
        if (!cat->allows_timetag) {
            pf_tm_nak(seq, "TIMETAG_INVALID", "command cannot be time tagged");
            return;
        }
        due = parse_hms(f[3]);
        if (due < 0) {
            pf_tm_nak(seq, "TIMETAG_INVALID", "time tag must be hh:mm:ss");
            return;
        }
        if (!S.obt_valid) {
            pf_tm_nak(seq, "OBT_NOT_SET", "set OBT first");
            return;
        }
        if (due <= pf_clock_now()) {
            pf_tm_nak(seq, "TIMETAG_OVERDUE", "time tag already passed");
            return;
        }
        future = 1;
    }

    /* 8, 9. mode and payload preconditions ------------------------- */
    why = pf_preconditions(cat->name, f[5]);
    if (why != NULL) {
        pf_tm_nak(seq, why, "preconditions not met");
        return;
    }

    /* 10. queue capacity ------------------------------------------- */
    if (future && pf_queue_add(due, seq, f[2], cat->name, f[5]) < 0) {
        pf_tm_nak(seq, "QUEUE_FULL", "time tag queue is full");
        return;
    }

    /* Accepted. */
    pf_tm_ack(seq, "ACCEPTED", cat->name);
    if (!future)
        pf_tc_execute(seq, cat->name, f[5]);
}

/* ------------------------------------------------------------------ */
/* Telemetry arriving from the payload                                 */
/* ------------------------------------------------------------------ */

static void pf_pl_handle(char *line)
{
    char  original[OBDH_MAX_LINE];
    char *f[OBDH_MAX_FIELDS];
    int   nf, seq;

    printf("[PF] <PL   %s\n", line);
    fflush(stdout);

    snprintf(original, sizeof(original), "%s", line);

    nf = split_fields(line, f, OBDH_MAX_FIELDS);
    if (nf < 5)
        return;

    seq = atoi(f[1]);
    if (S.last_pl_seq > 0 && seq > S.last_pl_seq + 1) {
        char extra[64];
        snprintf(extra, sizeof(extra), "expected=%d;got=%d", S.last_pl_seq + 1, seq);
        pf_tm_event("SEQ_GAP", extra);
    }
    S.last_pl_seq = seq;

    /* Housekeeping is forwarded to the ground, renumbered onto the
     * ground link but otherwise unchanged (ICD).                      */
    if (strcmp(f[0], "HK") == 0) {
        char fwd[OBDH_MAX_LINE];
        if (strstr(f[4], "pl_state=CAPTURING"))
            S.pl_state = PLS_CAPTURING;
        else if (strstr(f[4], "pl_state=IDLE"))
            S.pl_state = PLS_IDLE;
        else if (strstr(f[4], "pl_state=OFF"))
            S.pl_state = PLS_OFF;

        snprintf(fwd, sizeof(fwd), "HK|%d|PL|%s|%s", S.tm_seq++, f[3], f[4]);
        pf_tm_send(fwd);
        return;
    }

    if (strcmp(f[0], "EV") != 0)
        return;

    if (strstr(f[4], "event=PL_ON")) {
        S.pl_power = 1;
        S.pl_state = PLS_IDLE;
        pf_tm_event("PL_ON", NULL);
        if (S.pend_power_ref >= 0) {
            pf_tm_ack(S.pend_power_ref, "EXECUTED", "PL_POWER");
            S.pend_power_ref = -1;
        }
    } else if (strstr(f[4], "event=PL_OFF")) {
        S.pl_power = 0;
        S.pl_state = PLS_OFF;
        pf_tm_event("PL_OFF", NULL);
        if (S.pend_power_ref >= 0) {
            pf_tm_ack(S.pend_power_ref, "EXECUTED", "PL_POWER");
            S.pend_power_ref = -1;
        }
    } else if (strstr(f[4], "event=IMAGE_START")) {
        S.pl_state = PLS_CAPTURING;
        pf_tm_event("IMAGE_START", NULL);
    } else if (strstr(f[4], "event=IMAGE_DONE")) {
        const char *extra = strstr(f[4], "event=IMAGE_DONE");
        extra += strlen("event=IMAGE_DONE");
        if (*extra == ';')
            extra++;
        S.pl_state = PLS_IDLE;
        pf_tm_event("IMAGE_DONE", extra);
        if (S.pend_image_ref >= 0) {
            pf_tm_ack(S.pend_image_ref, "EXECUTED", "TAKE_IMAGE");
            S.pend_image_ref = -1;
        }
    }
}

/* ------------------------------------------------------------------ */
/* Link servicing                                                      */
/* ------------------------------------------------------------------ */

static void pf_link_accept(void)
{
    int fd;

    fd = tcp_accept(S.gnd_listen);
    if (fd >= 0) {
        if (S.gnd_fd >= 0)
            close(S.gnd_fd);            /* ICD: a new client replaces the old */
        S.gnd_fd = fd;
        lb_init(&S.gnd_buf);
        S.last_gnd_seq = 0;
        printf("[PF] ---- ground connected\n");
        fflush(stdout);
        pf_tm_event("PF_BOOT", NULL);
        pf_tm_hk();
    }

    fd = tcp_accept(S.pl_listen);
    if (fd >= 0) {
        if (S.pl_fd >= 0)
            close(S.pl_fd);
        S.pl_fd = fd;
        lb_init(&S.pl_buf);
        S.last_pl_seq = 0;
        printf("[PF] ---- payload connected\n");
        fflush(stdout);
        pf_tm_event("PL_LINK_UP", NULL);
        pf_pl_send("PL_PING", "");
        pf_pl_broadcast_time();          /* ICD: broadcast on PL connect */
    }
}

static void pf_link_read(int *fd, linebuf *lb, void (*handler)(char *))
{
    char    chunk[1024];
    char    line[OBDH_MAX_LINE];
    ssize_t n;
    int     r;

    if (*fd < 0)
        return;

    n = read(*fd, chunk, sizeof(chunk));
    if (n == 0) {
        close(*fd);
        *fd = -1;
        return;
    }
    if (n < 0)
        return;

    lb_feed(lb, chunk, (size_t)n);
    while ((r = lb_next(lb, line, sizeof(line))) != 0) {
        if (r < 0) {
            pf_tm_nak(0, "BAD_FORMAT", "line too long");
            continue;
        }
        if (line[0] != '\0')
            handler(line);
    }
}

/* ------------------------------------------------------------------ */
/* Main                                                                */
/* ------------------------------------------------------------------ */

int main(void)
{
    memset(&S, 0, sizeof(S));
    S.gnd_fd         = -1;
    S.pl_fd          = -1;
    S.mode           = M_SAFE;
    S.pend_mode_ref  = -1;
    S.pend_power_ref = -1;
    S.pend_image_ref = -1;
    S.tm_seq         = 1;
    S.pl_tc_seq      = 1;
    S.boot_mono      = mono_ms();
    lb_init(&S.gnd_buf);
    lb_init(&S.pl_buf);

    S.gnd_listen = tcp_listen(PF_GND_PORT);
    S.pl_listen  = tcp_listen(PF_PL_PORT);
    if (S.gnd_listen < 0 || S.pl_listen < 0) {
        fprintf(stderr, "[PF] cannot bind ports %d / %d\n",
                PF_GND_PORT, PF_PL_PORT);
        return 1;
    }

    printf("[PF] ---- booted, SAFE, SCET running, listening on %d (Gnd) and %d (PL)\n",
           PF_GND_PORT, PF_PL_PORT);
    fflush(stdout);

    S.next_hk_mono = mono_ms() + HK_PERIOD_MS;

    for (;;) {
        struct pollfd p[4];
        int           n = 0;

        p[n].fd = S.gnd_listen; p[n].events = POLLIN; n++;
        p[n].fd = S.pl_listen;  p[n].events = POLLIN; n++;
        if (S.gnd_fd >= 0) { p[n].fd = S.gnd_fd; p[n].events = POLLIN; n++; }
        if (S.pl_fd  >= 0) { p[n].fd = S.pl_fd;  p[n].events = POLLIN; n++; }

        poll(p, (nfds_t)n, TICK_MS);

        pf_link_accept();
        pf_link_read(&S.gnd_fd, &S.gnd_buf, pf_tc_handle);

        if (S.pl_fd >= 0) {
            int was = S.pl_fd;
            pf_link_read(&S.pl_fd, &S.pl_buf, pf_pl_handle);
            if (S.pl_fd < 0 && was >= 0) {
                S.pl_power = 0;
                S.pl_state = PLS_OFF;
                printf("[PF] ---- payload link lost\n");
                fflush(stdout);
                pf_tm_event("PL_LINK_DOWN", NULL);
            }
        }

        pf_mode_service();
        pf_queue_service();

        if (mono_ms() >= S.next_hk_mono) {
            S.next_hk_mono += HK_PERIOD_MS;
            pf_tm_hk();
            pf_pl_broadcast_time();      /* ICD: broadcast once per HK cycle */
        }
    }

    return 0;
}
