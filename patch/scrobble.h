/* M1+ Music Last.fm scrobbling (patch/scrobble.c; docs/modules/scrobble.md).
 *
 * The status bar's 1 s tick (the battery hook, patch/battery.c bp_capacity) calls lf_tick. With CARD_DIR/lastfm present (tools/lastfm_login.py writes it), a track the
 * player plays for half its length or 4 minutes (at least 30 s long) is appended to CARD_DIR/scrobbles, and while the Wi-Fi is up one detached thread posts the queue
 * to Last.fm's track.scrobble, at most LF_BATCH plays a request. Without the file the tick does nothing but count down to its next look.
 */
#pragma once

#include "covercache.h"

#define LF_CFG CARD_DIR "/lastfm" /* api_key=, secret=, sk= lines (tools/lastfm_login.py); absent: scrobbling is off */
#define LF_QUEUE \
    CARD_DIR \
    "/scrobbles" /* the plays not yet sent, one a line: timestamp, length, artist, title, album, album artist (tabs) */
#define LF_QTMP CARD_DIR "/scrobbles.tmp"
#define LF_URL "https://ws.audioscrobbler.com/2.0/"

/* The worker's answers (lf.res). */
enum {
    LF_R_OK = 1, /* Last.fm took the batch (status ok; plays it ignored, too old for one, are not sent again) */
    LF_R_RETRY, /* no answer, a time-out, an HTTP error without a Last.fm error, or Last.fm busy (11, 16, 29): sent again later */
    LF_R_AUTH, /* the key or the session was refused (4, 9, 10, 13, 26): nothing sent until LF_AUTH_WAIT or a new LF_CFG */
    LF_R_DROP, /* the batch itself was refused (6, 7): dropped, so one bad line never blocks the queue */
};

void lf_tick(void); /* the status bar's tick: the play watch and the sender (UI thread) */
