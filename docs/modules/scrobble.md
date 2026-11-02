# Last.fm scrobbling

The player sends what it plays to Last.fm. Plays wait on the card while there is no Wi-Fi
and go out once it is up.

Sources: `patch/scrobble.c`, `patch/scrobble.h`, the call in `patch/battery.c` `bp_capacity`,
`LF_*` in `patch/offsets.inc`, `tools/lastfm_login.py` (`make lastfm`). Tests:
`test/pod/scrobble.py`.

## Sign-in

Last.fm needs an API account of the user's own (key and shared secret,
https://www.last.fm/api/account/create). With the card mounted on the computer:

```fish
make lastfm VOLUME=/run/media/$USER/SHANLING
```

asks for the key, the secret, the user name and the password, calls
`auth.getMobileSession` over HTTPS and writes `/mnt/mmc/.m1plus/lastfm`:

```
api_key=<32 hex>
secret=<32 hex>
sk=<session key>
```

The password is never stored; the session key does not expire. `make lastfm VOLUME=... OFF=1`
removes the file. Without the file scrobbling is off. The file is plain text on the card:
whoever holds the card can scrobble as the account.

## The tick

`lf_tick` runs on the status bar's 1 s timer (`show_sysbar_face`, hook 8 in
[payload](payload.md), shared with the [battery percentage](battery.md)). That timer runs on
every page; stock skips it only in standby (`standby_mode` 2). The call is inside
`bp_capacity`'s kill-switch test, so `/mnt/mmc/m1plus-off` stops it.

This is the one feature whose work is not owned by a page: a play has to be seen whatever
page is in front and with the screen off. Off (no sign-in file), a tick only counts down; the
file is looked for once a minute (`LF_CFG_TICKS`). On, it is read again once a minute, so a
removed or new sign-in takes effect within a minute.

## What counts as a play

Each tick reads the playing track (`cc_playing`), its position and length
(`get_playtimeandlength`) and the play state (`mclGetPlayStatus`, 2 playing).

- A play starts when the track changes (path and CUE start) or the position goes back into
  the first `LF_RESTART` (5) seconds from later on (repeat, previous).
- Seconds count while playing, when the position moved forward by no more than the wall clock
  did since the last tick plus 2 s: a seek forward does not count, a late tick does.
- Last.fm's rules: a track of at least 30 s (`LF_MIN_LEN`) is queued once a play, when half its
  length or 4 minutes (`LF_MAX_WAIT`) has played, with the time the play started.
- Tags: title, artist, album and album artist (`REC_ALBUM_ARTIST` +0x24) of the queue's record;
  a record without artist or title takes the playing file's own tags (`playsong_id3info`
  album +0x404, artist +0x504, title +0x684) when its path is the record's.
- Not queued: podcast episodes (`pc_show_of`), a play without artist or title, a clock before
  `PN_MIN_EPOCH` (never set).

Timestamps are `time()`: they are right when the player's clock and time zone are.

## The queue

`/mnt/mmc/.m1plus/scrobbles`, one line a play: `start, length, artist, title, album, album
artist`, tab-separated (a tab or line break in a tag becomes a space; a tag is cut to
`LF_FIELD` bytes, never inside a UTF-8 character). The UI thread appends with `O_APPEND`, only
with the card mounted, up to `LF_QCAP` (256 KB, some 1500 plays); a play beyond that is not
kept.

## Sending

With plays queued, the time come (`lf.next`), a set clock, Wi-Fi (`get_wifisignal` > 0), the CA
bundle and the card, the UI thread reads the first `LF_BATCH` (50) lines (at most `LF_SNAP`
bytes), keeps them, and starts one detached thread with a copy and the sign-in. Without Wi-Fi
the next look is `LF_IDLE_LOOK` (30 s) later.

The thread builds `track.scrobble` with `timestamp[i]`, `duration[i]`, `artist[i]`, `track[i]`
and, when not empty, `album[i]` and `albumArtist[i]`, plus `api_key`, `method` and `sk`. The
signature `api_sig` is the MD5 (`lf_md5`, RFC 1321) of the parameters sorted by name, each name
then its value, then the secret. The form is POSTed over HTTPS to
`https://ws.audioscrobbler.com/2.0/` (libcurl, `PN_CA`, HTTPS only, 20 s connect, 60 s
total). A line without its six fields is left out. The thread touches nothing but its job and
libcurl; it sets `lf.res` and then clears `lf.busy`.

The next tick acts on the answer:

| Answer | Then |
|---|---|
| `status="ok"` | the lines sent leave the queue (when the file still starts with them; plays appended meanwhile stay), the next batch at once |
| error 6, 7 (invalid parameters) | the lines are dropped, so one bad line never blocks the queue |
| error 4, 9, 10, 13, 26 (key or session refused) | nothing for `LF_AUTH_WAIT` (1 h), or until a new session is on the card |
| anything else (no connection, time-out, 11, 16, 29) | sent again after 60 s, doubling to 30 min |

Last.fm accepts plays up to 14 days old; older ones come back as ignored with `status="ok"` and
leave the queue.

## Validation limits

The emulator covers the play rules, the queue, the request (its signature checked against
Python's MD5 at many lengths), the answers and the back-off. Not yet seen on the device: the
status bar tick with the screen off during playback, the clock and time zone of the
timestamps, Last.fm's answers to real requests. Device check EXTRA-07.
