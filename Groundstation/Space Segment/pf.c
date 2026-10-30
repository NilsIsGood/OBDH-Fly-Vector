/* pf.c - Platform Onboard Computer (PF) for the R7023E OBDH model.
 *
 * Note: I'm not going to pretend to fully understand this well enough to rebuild it from memory, but . . . 
 * I "built" it (painfully) over the course of several weeks (meaning about 3 panic-fueled midnight code and then vibe-coding sessions)
 * It works now. That wasn't always the case. I take full responsibilty for everything in here that seems completely insane.
 * One day I'll make proper sense of it all . . . but it is not this day ~Sun Tzu probably
 * For now I've tried to comment things well enough to be interpretable for you the reader / assignment assesor
 * I largely built this when I was smarter person ( . . . before exam week). So commenting it now, is a challenge and might be messy.
 * Also . . . I started this in c++ (do with that information what you will)
 * ~ Precious
 *
 * Implements ICD v0.1. PF is the hub: it listens on two TCP ports,
 * validates everything arriving from the ground, schedules time-tagged
 * commands, and relays simplified instructions to the payload.
 * It's basically a micromanager.
 */

#include "obdh.h"
#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define TICK_MS              100   
#define HK_PERIOD_MS        2000   
#define MODE_TRANSITION_MS  8000   
#define QUEUE_SIZE            16   

enum mode { M_SAFE = 0, M_TRANSITION, M_TARGET };
static const char *mode_name[] = { "SAFE", "TRANSITION", "TARGET" };

/* PF mirrors the payload state purely so that it can reject TAKE_IMAGE
 * with PL_BUSY without bothering the actual payload. */
enum pl_state { PLS_OFF = 0, PLS_IDLE, PLS_CAPTURING };

// Struct for a time-tagged command resting in our queue
typedef struct {
    int  used;
    long due;              /* seconds of day, OBT when this should run */
    int  ref_seq;          /* ground sequence number that birthed it */
    char dest[4];
    char cmd[32];
    char args[64];
} queue_entry;

// Global state beast
static struct {
    int     gnd_listen, pl_listen;
    int     gnd_fd, pl_fd;
    linebuf gnd_buf, pl_buf;
    long boot_mono;
    int  obt_valid;
    long obt_offset;       
    enum mode     mode;
    enum mode     mode_target;
    long          mode_deadline;   
    int           pend_mode_ref;   
    int           pl_power;        
    enum pl_state pl_state;
    int           pend_power_ref;
    int           pend_image_ref;
    int  tm_seq;           
    int  pl_tc_seq;        
    int  last_gnd_seq;     
    int  last_pl_seq;      
    queue_entry queue[QUEUE_SIZE];
    long        next_hk_mono;
} S;


/* ------------------------------------------------------------------ */
/* Preconditions (ICD checks 8 and 9)                                  */
/*                                                                     */
/* Evaluated TWICE: once at uplink for the acceptance decision, and    */
/* again at execution time before the command is dispatched, because   */
/* the universe (and our mode state) changes while commands sit in the queue.*/
/* ------------------------------------------------------------------ */

// [TCV] Returns a reason code string if preconditions fail, or NULL if we are good to go.
static const char *pf_preconditions(const char *cmd, const char *args)
{
    if (strcmp(cmd, "SET_MODE") == 0) {
        if (S.mode == M_TRANSITION) return "IN_TRANSITION"; // Already moving, please hold
        return NULL;
    }
    if (strcmp(cmd, "PL_POWER") == 0) {
        if (S.pl_fd < 0) return "PL_LINK_DOWN"; // Payload is MIA
        if (strcmp(args, "ON") == 0 && S.mode != M_TARGET) return "WRONG_MODE"; // Gotta be in Target mode
        return NULL;
    }
    if (strcmp(cmd, "TAKE_IMAGE") == 0) {
        if (S.mode != M_TARGET) return "WRONG_MODE"; 
        if (S.pl_fd < 0) return "PL_LINK_DOWN";
        if (!S.pl_power) return "PL_OFF"; // You can't take a picture if the camera is off ~ Some Zen proverb . . . probably
        if (S.pl_state == PLS_CAPTURING) return "PL_BUSY"; // Already taking a picture!
        return NULL;
    }
    return NULL;
}


/* ------------------------------------------------------------------ */
/* Validation (ICD checks 1 to 10, in strict order)                    */
/* ------------------------------------------------------------------ */

// [TCV] The gauntlet. Every incoming string from Ground must pass these 10 trials.
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

    /* 1. Format: Are you even speaking my language? Like do you even Finnish, bro? */
    if (!is_printable_ascii(line)) {
        pf_tm_nak(0, "BAD_FORMAT", "non printable characters");
        return;
    }
    nf = split_fields(line, f, OBDH_MAX_FIELDS);
    if (nf != 6 || strcmp(f[0], "TC") != 0) {
        pf_tm_nak(0, "BAD_FORMAT", "expected 6 fields or not a telecommand");
        return;
    }

    /* 2. Sequence: Did we miss a packet or get a duplicate? */
    seq = atoi(f[1]);
    if (seq <= 0) {
        pf_tm_nak(0, "SEQ_ERROR", "sequence not parseable");
        return;
    }
    if (seq <= S.last_gnd_seq) {
        // Ground sent an old or duplicate packet. We must reject it to avoid doing things twice.
        pf_tm_nak(seq, "SEQ_ERROR", "duplicate or out of order");
        return;
    }
    if (S.last_gnd_seq > 0 && seq > S.last_gnd_seq + 1) {
        /* Oh no . . . a gap. We report the gap so Ground knows, but we ACCEPT the 
         * command anyway because stopping the whole demo for a network hiccup is bad m'kay (South Park Reference . . . I'm dating myself now). */
        char extra[64];
        snprintf(extra, sizeof(extra), "expected=%d;got=%d", S.last_gnd_seq + 1, seq);
        pf_tm_event("SEQ_GAP", extra);   
    }
    S.last_gnd_seq = seq;

    /* 3. Catalogue: Do we actually know this command? */
    cat = pf_catalogue_find(f[4]);
    if (cat == NULL) {
        pf_tm_nak(seq, "UNKNOWN_CMD", "not in catalogue");
        return;
    }

    /* 4. Arguments: Did you provide the right variables? */
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
    // Hardcoded logic for specific commands because C lacks polymorphism . . . Unlike C++!!!
    if (strcmp(cat->name, "SET_MODE") == 0 &&
        strcmp(f[5], "SAFE") != 0 && strcmp(f[5], "TARGET") != 0) {
        pf_tm_nak(seq, "BAD_ARG", "mode must be SAFE or TARGET");
        return;
    }

    /* 5, 6, 7. Time tag: Do we have a time machine? */
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
        future = 1; // It's a time-tagged command! Mark it for the queue.
    }

    /* 8, 9. Mode and Payload preconditions: Is it physically possible right now? */
    why = pf_preconditions(cat->name, f[5]);
    if (why != NULL) {
        pf_tm_nak(seq, why, "preconditions not met");
        return;
    }

    /* 10. Queue capacity: Do we have room for this time-tagged command? */
    if (future && pf_queue_add(due, seq, f[2], cat->name, f[5]) < 0) {
        pf_tm_nak(seq, "QUEUE_FULL", "time tag queue is full");
        return;
    }

    /* If we made it here, the command passed the gauntlet. */
    pf_tm_ack(seq, "ACCEPTED", cat->name);
    
    // If it's an immediate command, DO IT NOW. If not, grab a cold one or go touch grass or something
    if (!future)
        pf_tc_execute(seq, cat->name, f[5]);
}


/* ------------------------------------------------------------------ */
/* Time-tag queue execution                                            */
/* ------------------------------------------------------------------ */

// [QUE] The janitor function. Called every 100ms tick to see if any queued commands are due.
static void pf_queue_service(void)
{
    long now = pf_clock_now();
    int  i;

    if (!S.obt_valid)
        return; // Clock isn't set, we can't process time-tags.

    for (i = 0; i < QUEUE_SIZE; i++) {
        queue_entry *q = &S.queue[i];
        const char  *why;
        char         extra[128];

        // Skip empty slots or commands from the future
        if (!q->used || q->due > now)
            continue;

        q->used = 0; // Pop it off the queue

        snprintf(extra, sizeof(extra), "ref=%d;cmd=%s", q->ref_seq, q->cmd);
        pf_tm_event("TC_DUE", extra);

        /* ICD rule 3: RE-CHECK PRECONDITIONS AT EXECUTION. 
         * A failure here is an event (TC_FAILED), not a NAK, because 
         * we already ACCEPTED it on uplink. But the world changed. */
        why = pf_preconditions(q->cmd, q->args);
        if (why != NULL) {
            snprintf(extra, sizeof(extra), "ref=%d;reason=%s", q->ref_seq, why);
            pf_tm_event("TC_FAILED", extra);
            continue; // Abandon ship
        }

        // It survived the second check. Send it.
        pf_tc_execute(q->ref_seq, q->cmd, q->args);
    }
}