/* M1+ Music Last.fm scrobbling (docs/modules/scrobble.md).
 *
 * The play watch (UI thread, lf_tick from the status bar's 1 s tick): the playing track (cc_playing), its position and length (get_playtimeandlength) and the play
 * state (mclGetPlayStatus, 2 = playing). Seconds count as played when the position moved forward by no more than the wall clock did since the last look (+ 2 s), so a
 * seek forward does not count and a slow tick does. A play starts when the track changes or the position goes back to the first LF_RESTART seconds. A play of a track of
 * at least LF_MIN_LEN seconds is queued once, when its played seconds reach half the length or LF_MAX_WAIT, with the time it started; podcast episodes, a track without
 * an artist or a title, and a clock that was never set are not queued.
 *
 * The queue (CARD_DIR/scrobbles): text lines appended by the UI thread (O_APPEND), at most LF_QCAP bytes (a play beyond it is not kept). The sender takes the first
 * LF_BATCH lines (at most LF_SNAP bytes) and hands a copy to one detached thread; the thread signs and posts them and only sets lf.res and clears lf.busy. The UI thread
 * then removes those lines (when the file still starts with them), or waits LF_RETRY_MIN doubling to LF_RETRY_MAX before trying again. The worker touches no stock
 * function, widget or file: only its own job and libcurl.
 */
#include "offsets.inc"
#include "stock.h"
#include "common.h"
#include "covercache.h"
#include "curlopts.h"
#include "podcasts.h"
#include "pcfeed.h"
#include "pcjson.h"
#include "pcnet.h"
#include "scrobble.h"

static struct {
    int on; /* LF_CFG read and complete */
    int wait; /* ticks until the next look for LF_CFG while off */
    int age; /* ticks since LF_CFG was last read while on */
    int pending; /* the queue may hold lines */
    char key[LF_CFG_VAL], secret[LF_CFG_VAL], sk[LF_CFG_VAL];
    /* the play watch */
    unsigned track; /* the playing track's hash (path, CUE start); 0 none */
    int pos; /* its position at the last look */
    unsigned at, start, played; /* the wall clock at the last look, when the play started, the seconds played */
    int queued; /* this play is in the queue */
    /* the sender */
    char *snap; /* the lines the worker has (UI thread's copy), NULL when none */
    unsigned snapn;
    unsigned next, delay; /* no send before the wall clock reaches next; the current back-off */
    volatile int busy, res;
} lf __attribute__((section(".scratch")));

struct lf_job {
    char *text; /* the lines, the worker's own copy */
    unsigned n;
    char key[LF_CFG_VAL], secret[LF_CFG_VAL], sk[LF_CFG_VAL];
};

static unsigned lf_len(const char *s) {
    unsigned n = 0;
    while (s[n]) ++n;
    return n;
}

static int lf_starts(const char *s, const char *p) {
    while (*p)
        if (*s++ != *p++) return 0;
    return 1;
}

/* The first place p occurs in s[0..n), or NULL. */
static const char *lf_find(const char *s, unsigned n, const char *p) {
    unsigned m = lf_len(p), i, k;
    for (i = 0; i + m <= n; ++i) {
        for (k = 0; k < m && s[i + k] == p[k]; ++k) {
        }
        if (k == m) return s + i;
    }
    return (const char *)0;
}

/* ---------------------------------------------------------------------------
 * LF_CFG
 */

/* The value after "name=" on a line of t (to the end of the line, spaces and CR trimmed) into out; 1 when there is one that fits. */
static int lf_cfg_val(const char *t, const char *name, char *out) {
    const char *p = t;
    unsigned n;
    while (*p) {
        if (lf_starts(p, name) && p[lf_len(name)] == '=') {
            p += lf_len(name) + 1;
            for (n = 0; p[n] && p[n] != '\n' && p[n] != '\r' && p[n] != ' '; ++n)
                if (n + 1 >= LF_CFG_VAL) return 0;
            memcpy(out, p, n);
            out[n] = 0;
            return n > 0;
        }
        while (*p && *p != '\n') ++p;
        if (*p) ++p;
    }
    return 0;
}

/* Read LF_CFG: on when it has the key, the secret and the session. A new session lifts a wait the old one's refusal set. */
static void lf_load(void) {
    char t[LF_CFG_MAX + 1], sk[LF_CFG_VAL];
    int fd, n;
    lf.on = 0;
    if (access(LF_CFG, F_OK) != 0 || (fd = open(LF_CFG, O_RDONLY, 0)) < 0) return;
    n = read(fd, t, LF_CFG_MAX);
    close(fd);
    if (n <= 0) return;
    t[n] = 0;
    if (!lf_cfg_val(t, "api_key", lf.key) || !lf_cfg_val(t, "secret", lf.secret) || !lf_cfg_val(t, "sk", sk)) return;
    if (strcmp(sk, lf.sk) != 0) {
        memcpy(lf.sk, sk, sizeof sk);
        lf.next = 0, lf.delay = 0, lf.pending = 1; /* a new login: try the queue at once */
    }
    lf.on = 1;
}

/* ---------------------------------------------------------------------------
 * The play watch
 */

/* s into out (at most LF_FIELD bytes, never cutting a UTF-8 character), a tab or a line break becoming a space; returns the bytes written. */
static unsigned lf_field(char *out, const char *s) {
    unsigned n = 0;
    while (s[n] && n < LF_FIELD) ++n;
    if (s[n])
        while (n && ((unsigned char)s[n] & 0xc0) == 0x80)
            --n; /* s[n] continues a character: cut before the character */
    for (unsigned i = 0; i < n; ++i) out[i] = s[i] == '\t' || s[i] == '\n' || s[i] == '\r' ? ' ' : s[i];
    return n;
}

/* The play into the queue: "start\tlength\tartist\ttitle\talbum\talbum artist\n". Tags from the record, else from the tags stock read for the playing file. */
static void lf_queue(void *r, int len) {
    const char *path = cc_str(r, REC_PATH), *artist = cc_str(r, REC_ARTIST), *title = cc_str(r, REC_TITLE);
    const char *album = cc_str(r, REC_ALBUM), *aartist = cc_str(r, REC_ALBUM_ARTIST);
    const char *info = (const char *)PLAYSONG_ID3INFO;
    char line[LF_LINE];
    unsigned o;
    int fd;
    long size;
    if ((!*artist || !*title) && strcmp(info, path) == 0) {
        if (!*artist) artist = info + ID3_ARTIST;
        if (!*title) title = info + ID3_TITLE;
        if (!*album) album = info + ID3_ALBUM;
    }
    if (!*artist || !*title || pc_show_of(path) || !pod_card_mounted()) return;
    o = (unsigned)snprintf(line, sizeof line, "%u\t%d\t", lf.start, len);
    o += lf_field(line + o, artist), line[o++] = '\t';
    o += lf_field(line + o, title), line[o++] = '\t';
    o += lf_field(line + o, album), line[o++] = '\t';
    o += lf_field(line + o, aartist), line[o++] = '\n';
    fd = open(LF_QUEUE, O_WRONLY | O_APPEND | O_CREAT, 0644);
    if (fd < 0) return;
    size = lseek(fd, 0, 2);
    if (size >= 0 && (unsigned long)size + o <= LF_QCAP && write(fd, line, o) == (int)o) lf.pending = 1;
    close(fd);
}

static unsigned lf_hash(const char *p, int cue) {
    unsigned h = 2166136261u ^ (unsigned)cue;
    while (*p) h = (h ^ (unsigned char)*p++) * 16777619u;
    return h ? h : 1;
}

static void lf_watch(void) {
    void *r = cc_playing();
    int pos = 0, len = 0, played;
    unsigned key, now;
    if (!r || get_playtimeandlength(&pos, &len) != 1 || len <= 0 || pos < 0) return;
    key = lf_hash(cc_str(r, REC_PATH), *(int *)((char *)r + REC_CUE_START));
    now = (unsigned)time((long *)0);
    if (key != lf.track || (pos + LF_RESTART < lf.pos && pos <= LF_RESTART)) { /* a new play */
        lf.track = key, lf.pos = pos, lf.at = now, lf.start = now - (unsigned)pos, lf.played = 0, lf.queued = 0;
        return;
    }
    played = pos - lf.pos;
    if (mclGetPlayStatus() == 2 && played > 0 && (unsigned)played <= now - lf.at + 2) lf.played += (unsigned)played;
    lf.pos = pos, lf.at = now;
    if (!lf.queued && len >= LF_MIN_LEN && lf.played >= (unsigned)(len / 2 < LF_MAX_WAIT ? len / 2 : LF_MAX_WAIT)) {
        lf.queued = 1;
        if (lf.start >= PN_MIN_EPOCH) lf_queue(r, len);
    }
}

/* ---------------------------------------------------------------------------
 * MD5 (RFC 1321) for the request signature
 */
static const unsigned lf_k[64] = {
    0xd76aa478, 0xe8c7b756, 0x242070db, 0xc1bdceee, 0xf57c0faf, 0x4787c62a, 0xa8304613, 0xfd469501,
    0x698098d8, 0x8b44f7af, 0xffff5bb1, 0x895cd7be, 0x6b901122, 0xfd987193, 0xa679438e, 0x49b40821,
    0xf61e2562, 0xc040b340, 0x265e5a51, 0xe9b6c7aa, 0xd62f105d, 0x02441453, 0xd8a1e681, 0xe7d3fbc8,
    0x21e1cde6, 0xc33707d6, 0xf4d50d87, 0x455a14ed, 0xa9e3e905, 0xfcefa3f8, 0x676f02d9, 0x8d2a4c8a,
    0xfffa3942, 0x8771f681, 0x6d9d6122, 0xfde5380c, 0xa4beea44, 0x4bdecfa9, 0xf6bb4b60, 0xbebfbc70,
    0x289b7ec6, 0xeaa127fa, 0xd4ef3085, 0x04881d05, 0xd9d4d039, 0xe6db99e5, 0x1fa27cf8, 0xc4ac5665,
    0xf4292244, 0x432aff97, 0xab9423a7, 0xfc93a039, 0x655b59c3, 0x8f0ccc92, 0xffeff47d, 0x85845dd1,
    0x6fa87e4f, 0xfe2ce6e0, 0xa3014314, 0x4e0811a1, 0xf7537e82, 0xbd3af235, 0x2ad7d2bb, 0xeb86d391};
static const unsigned char lf_s[16] = {7, 12, 17, 22, 5, 9, 14, 20, 4, 11, 16, 23, 6, 10, 15, 21};

static void lf_md5_block(unsigned *h, const unsigned char *p) {
    unsigned w[16], a = h[0], b = h[1], c = h[2], d = h[3], f, g, t, i;
    for (i = 0; i < 16; ++i)
        w[i] = (unsigned)p[i * 4] | (unsigned)p[i * 4 + 1] << 8 | (unsigned)p[i * 4 + 2] << 16 |
               (unsigned)p[i * 4 + 3] << 24;
    for (i = 0; i < 64; ++i) {
        if (i < 16)
            f = (b & c) | (~b & d), g = i;
        else if (i < 32)
            f = (d & b) | (~d & c), g = (5 * i + 1) & 15;
        else if (i < 48)
            f = b ^ c ^ d, g = (3 * i + 5) & 15;
        else
            f = c ^ (b | ~d), g = (7 * i) & 15;
        t = d, d = c, c = b;
        f += a + lf_k[i] + w[g];
        b += f << lf_s[(i >> 4) * 4 + (i & 3)] | f >> (32 - lf_s[(i >> 4) * 4 + (i & 3)]);
        a = t;
    }
    h[0] += a, h[1] += b, h[2] += c, h[3] += d;
}

/* The MD5 of m[0..n) as 32 lowercase hex digits and a NUL into hex. */
static void lf_md5(const char *m, unsigned n, char *hex) {
    unsigned h[4] = {0x67452301, 0xefcdab89, 0x98badcfe, 0x10325476}, i, rem, tn;
    unsigned char tail[128];
    for (i = 0; i + 64 <= n; i += 64) lf_md5_block(h, (const unsigned char *)m + i);
    rem = n - i;
    memcpy(tail, m + i, rem);
    tail[rem] = 0x80;
    tn = rem + 9 <= 64 ? 64 : 128;
    memset(tail + rem + 1, 0, tn - rem - 1);
    for (i = 0; i < 4; ++i) tail[tn - 8 + i] = (unsigned char)((n << 3) >> (8 * i));
    tail[tn - 4] = (unsigned char)(n >> 29);
    lf_md5_block(h, tail);
    if (tn == 128) lf_md5_block(h, tail + 64);
    for (i = 0; i < 16; ++i) {
        unsigned char v = (unsigned char)(h[i / 4] >> (8 * (i & 3)));
        hex[i * 2] = "0123456789abcdef"[v >> 4];
        hex[i * 2 + 1] = "0123456789abcdef"[v & 15];
    }
    hex[32] = 0;
}

/* ---------------------------------------------------------------------------
 * The worker: the request, signed (the parameters sorted by name, each name then its value, then the secret: api_sig), posted as a form
 */
struct lf_param {
    const char *name, *val;
};

struct lf_answer {
    char *buf;
    unsigned n;
};

static unsigned lf_write(char *d, unsigned size, unsigned nmemb, void *user) {
    struct lf_answer *a = user;
    unsigned len = size * nmemb, k = len;
    if (k > LF_ANSWER - a->n) k = LF_ANSWER - a->n;
    memcpy(a->buf + a->n, d, k);
    a->n += k;
    return len;
}

/* s, form-encoded (all but A-Z a-z 0-9 - _ . ~ as %XX), at out; returns the bytes written. */
static unsigned lf_esc(char *out, const char *s) {
    unsigned o = 0;
    for (; *s; ++s) {
        unsigned char c = (unsigned char)*s;
        if ((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '-' || c == '_' ||
            c == '.' || c == '~')
            out[o++] = (char)c;
        else
            out[o++] = '%', out[o++] = "0123456789ABCDEF"[c >> 4], out[o++] = "0123456789ABCDEF"[c & 15];
    }
    return o;
}

/* The answer's meaning: status ok, else Last.fm's error code, else (no answer, another HTTP error) a retry. */
static int lf_answer(const char *b, unsigned n, int rc) {
    const char *e;
    int code = 0;
    if (rc == 0 && lf_find(b, n, "status=\"ok\"")) return LF_R_OK;
    e = lf_find(b, n, "<error code=\"");
    if (!e) return LF_R_RETRY;
    for (e += 13; e < b + n && *e >= '0' && *e <= '9'; ++e) code = code * 10 + (*e - '0');
    if (code == 4 || code == 9 || code == 10 || code == 13 || code == 26) return LF_R_AUTH;
    if (code == 6 || code == 7) return LF_R_DROP;
    return LF_R_RETRY;
}

/* The job's lines as one track.scrobble request; a line without its six fields is left out. Returns an LF_R_* (OK when there is nothing to send). */
static int lf_post(struct lf_job *j) {
    static const char *const names[LF_FIELDS] = {"timestamp", "duration", "artist", "track", "album", "albumArtist"};
    struct lf_param *p = calloc(LF_BATCH * LF_FIELDS + 3, sizeof *p);
    char (*nm)[16] = calloc(LF_BATCH * LF_FIELDS, 16);
    char *f[LF_FIELDS], *s = j->text, *sig = 0, *body = 0, hex[33], *ans = calloc(LF_ANSWER + 1, 1);
    unsigned np = 0, lines = 0, i, k, need = 0, o;
    struct lf_answer a;
    void *h;
    int rc = -1, res = LF_R_RETRY;
    if (!p || !nm || !ans) goto out;
    while (*s && lines < LF_BATCH) {
        char *eol = s;
        while (*eol && *eol != '\n') ++eol;
        for (k = 0, f[0] = s; s < eol; ++s)
            if (*s == '\t' && k + 1 < LF_FIELDS) *s = 0, f[++k] = s + 1;
        if (*eol) *eol++ = 0;
        s = eol;
        if (k + 1 != LF_FIELDS || !*f[0] || !*f[2] || !*f[3]) continue;
        for (k = 0; k < LF_FIELDS; ++k) {
            if (!*f[k]) continue; /* no album, no album artist: not sent */
            snprintf(nm[lines * LF_FIELDS + k], 16, "%s[%u]", names[k], lines);
            p[np].name = nm[lines * LF_FIELDS + k], p[np++].val = f[k];
        }
        ++lines;
    }
    if (!lines) {
        res = LF_R_OK;
        goto out;
    }
    p[np].name = "api_key", p[np++].val = j->key;
    p[np].name = "method", p[np++].val = "track.scrobble";
    p[np].name = "sk", p[np++].val = j->sk;
    for (i = 1; i < np; ++i) { /* insertion sort by name (byte order, as Last.fm sorts) */
        struct lf_param t = p[i];
        for (k = i; k > 0 && strcmp(p[k - 1].name, t.name) > 0; --k) p[k] = p[k - 1];
        p[k] = t;
    }
    for (i = 0; i < np; ++i) need += lf_len(p[i].name) + lf_len(p[i].val);
    sig = calloc(need + lf_len(j->secret) + 1, 1);
    body = calloc(need * 3 + np * 2 + 64, 1);
    if (!sig || !body) goto out;
    for (i = o = 0; i < np; ++i) {
        memcpy(sig + o, p[i].name, lf_len(p[i].name)), o += lf_len(p[i].name);
        memcpy(sig + o, p[i].val, lf_len(p[i].val)), o += lf_len(p[i].val);
    }
    memcpy(sig + o, j->secret, lf_len(j->secret)), o += lf_len(j->secret);
    lf_md5(sig, o, hex);
    for (i = o = 0; i < np; ++i) {
        o += lf_esc(body + o, p[i].name), body[o++] = '=';
        o += lf_esc(body + o, p[i].val), body[o++] = '&';
    }
    memcpy(body + o, "api_sig=", 8), o += 8;
    memcpy(body + o, hex, 32), o += 32;
    a.buf = ans, a.n = 0;
    h = curl_easy_init();
    if (!h) goto out;
    curl_easy_setopt(h, CURLOPT_URL, LF_URL);
    curl_easy_setopt(h, CURLOPT_POSTFIELDS, body);
    curl_easy_setopt(h, CURLOPT_POSTFIELDSIZE, (long)o);
    curl_easy_setopt(h, CURLOPT_WRITEFUNCTION, lf_write);
    curl_easy_setopt(h, CURLOPT_WRITEDATA, &a);
    curl_easy_setopt(h, CURLOPT_NOSIGNAL, 1);
    curl_easy_setopt(h, CURLOPT_PROTOCOLS, CURLPROTO_HTTPS);
    curl_easy_setopt(h, CURLOPT_CONNECTTIMEOUT, 20);
    curl_easy_setopt(h, CURLOPT_TIMEOUT, 60);
    curl_easy_setopt(h, CURLOPT_USERAGENT, "M1PlusMusic/1.0 (https://github.com/imfing/m1plus-mod)");
    curl_easy_setopt(h, CURLOPT_CAINFO, PN_CA);
    rc = curl_easy_perform(h);
    curl_easy_cleanup(h);
    res = lf_answer(ans, a.n, rc);
out:
    free(p), free(nm), free(sig), free(body), free(ans);
    return res;
}

static void *lf_main(void *arg) {
    struct lf_job *j = arg;
    int res = lf_post(j);
    free(j->text);
    free(j);
    lf.res = res;
    lf.busy = 0; /* last: the UI thread reads res after it sees this */
    return (void *)0;
}

/* ---------------------------------------------------------------------------
 * The sender (UI thread)
 */

/* The queue without its first n bytes, when it still starts with the lines sent (b): the rest written back atomically. */
static void lf_trim(const char *b, unsigned n) {
    char *t = calloc(LF_QCAP + 1, 1);
    int fd, got = 0, r;
    unsigned i;
    if (!t) return;
    fd = open(LF_QUEUE, O_RDONLY, 0);
    if (fd >= 0) {
        while ((unsigned)got < LF_QCAP && (r = read(fd, t + got, LF_QCAP - (unsigned)got)) > 0) got += r;
        close(fd);
        for (i = 0; i < n && i < (unsigned)got && t[i] == b[i]; ++i) {
        }
        if (i == n && pod_card_write(CARD_DIR, LF_QUEUE, LF_QTMP, t + n, (unsigned)got - n, (const void *)0, 0) == 0)
            lf.pending = (unsigned)got > n;
    }
    free(t);
}

/* A finished worker's answer: the lines sent (or refused) leave the queue; otherwise a wait. */
static void lf_collect(unsigned now) {
    if (lf.res == LF_R_OK || lf.res == LF_R_DROP) {
        lf_trim(lf.snap, lf.snapn);
        lf.delay = 0, lf.next = 0;
    } else if (lf.res == LF_R_AUTH)
        lf.next = now + LF_AUTH_WAIT;
    else {
        lf.delay = lf.delay ? (lf.delay * 2 < LF_RETRY_MAX ? lf.delay * 2 : LF_RETRY_MAX) : LF_RETRY_MIN;
        lf.next = now + lf.delay;
    }
    free(lf.snap);
    lf.snap = (char *)0;
}

/* The first lines of the queue to the worker, when there are lines, the time has come and the network can be reached. */
static void lf_send(unsigned now) {
    struct lf_job *j;
    unsigned long th = 0;
    int fd, n = 0, r;
    unsigned lines = 0, cut = 0, i;
    if (!lf.pending || now < lf.next) return;
    if (now < PN_MIN_EPOCH || get_wifisignal() <= 0 || access(PN_CA, F_OK) != 0 || !pod_card_mounted()) {
        lf.next = now + LF_IDLE_LOOK;
        return;
    }
    lf.snap = calloc(LF_SNAP + 1, 1);
    j = calloc(1, sizeof *j);
    if (!lf.snap || !j) goto fail;
    fd = open(LF_QUEUE, O_RDONLY, 0);
    if (fd >= 0) {
        while (n < LF_SNAP && (r = read(fd, lf.snap + n, LF_SNAP - (unsigned)n)) > 0) n += r;
        close(fd);
    }
    for (i = 0; i < (unsigned)n && lines < LF_BATCH; ++i)
        if (lf.snap[i] == '\n') cut = i + 1, ++lines;
    if (!cut) {
        lf.pending = 0; /* empty, or one line longer than LF_SNAP that cannot be: nothing to send */
        goto fail;
    }
    lf.snap[cut] = 0, lf.snapn = cut;
    j->text = calloc(cut + 1, 1);
    if (!j->text) goto fail;
    memcpy(j->text, lf.snap, cut);
    j->n = cut;
    memcpy(j->key, lf.key, sizeof j->key), memcpy(j->secret, lf.secret, sizeof j->secret),
        memcpy(j->sk, lf.sk, sizeof j->sk);
    curl_global_init(CURL_GLOBAL_ALL); /* counted, not thread safe: here, on the UI thread */
    lf.busy = 1, lf.res = 0;
    if (pthread_create(&th, (const void *)0, lf_main, j) == 0) {
        pthread_detach(th);
        return;
    }
    lf.busy = 0;
    free(j->text);
fail:
    free(j);
    free(lf.snap);
    lf.snap = (char *)0;
    lf.next = now + LF_RETRY_MIN;
}

void lf_tick(void) {
    unsigned now;
    if (lf.snap && !lf.busy) lf_collect((unsigned)time((long *)0));
    if (!lf.on) {
        if (lf.wait > 0) {
            --lf.wait;
            return;
        }
        lf.wait = LF_CFG_TICKS;
        lf_load();
        if (!lf.on) return;
        lf.age = 0;
    } else if (++lf.age >= LF_CFG_TICKS) {
        lf.age = 0;
        lf_load();
        if (!lf.on) return;
    }
    lf_watch();
    now = (unsigned)time((long *)0);
    if (lf.next > now + LF_AUTH_WAIT) lf.next = now; /* the clock went back */
    if (!lf.busy && !lf.snap) lf_send(now);
}
