"""Last.fm scrobbling (patch/scrobble.c, docs/modules/scrobble.md): the play watch on the status bar's tick, the queue on the card, and the worker's signed track.scrobble
request against a scripted server (the signature checked with Python's MD5)."""
import hashlib, os, random, urllib.parse
from harness import *   # noqa: F401,F403  (test/patch.py initialises the harness before loading a module)

CARD = '/mnt/mmc/.m1plus'
CFG, QUEUE, CA, KILL = CARD + '/lastfm', CARD + '/scrobbles', '/etc/m1plus-cacert.pem', '/mnt/mmc/m1plus-off'
KEY, SECRET, SK = '0123456789abcdef0123456789abcdef', 'fedcba9876543210fedcba9876543210', 'a1b2c3d4e5f60718293a4b5c6d7e8f90'
CONF = f'api_key={KEY}\nsecret={SECRET}\nsk={SK}\n'.encode()
T0 = 1800000000
INFO = pod.DATA['PLAYSONG_ID3INFO'][0]


class SM(Machine):
    status = 2              # mclGetPlayStatus: 2 playing, 3 paused
    pt = (0, 200)           # get_playtimeandlength: position, length (s); None fails

    def __init__(self):
        super().__init__()
        for n in ('mclGetPlayStatus', 'get_playtimeandlength'): self.handlers[pod.FUNCTIONS[n][2]] = n

    def _hook(self, u, address, size, _):
        name = self.handlers.get(address, '')
        a0, a1 = u.reg_read(UC_MIPS_REG_A0), u.reg_read(UC_MIPS_REG_A1)
        if name == 'mclGetPlayStatus': ret = self.status
        elif name == 'get_playtimeandlength':
            if self.pt: self.setword(a0, self.pt[0]); self.setword(a1, self.pt[1])
            ret = 1 if self.pt else 0
        else: return super()._hook(u, address, size, _)
        u.reg_write(UC_MIPS_REG_V0, ret & 0xffffffff)
        u.reg_write(UC_MIPS_REG_PC, u.reg_read(UC_MIPS_REG_RA))


def machine(conf=CONF, wifi=-1):
    m = SM()
    m.dirs.add(CARD)
    if conf is not None: m.fs[CFG] = conf
    m.fs[CA] = b'roots'
    m.wifi, m.wall = wifi, T0
    return m


def tick(m): return m.call(PSYMS['lf_tick'], count=400000000)


def track(m, title='Karma Police', artist='Radiohead', album='OK Computer', length=200, path='/mnt/mmc/m/1.flac', aartist=None):
    r = m.record(album=album, artist=artist, path=path, title=title, name=os.path.basename(path))
    if aartist is not None: m.setword(r + O['REC_ALBUM_ARTIST'], m.cstr(aartist))
    m.play([r], 0)
    m.pt = (0, length)
    return r


def run_for(m, secs, step=1):
    """The player plays on for secs seconds, the tick coming every step seconds."""
    for _ in range(secs // step):
        m.wall += step; m.pt = (m.pt[0] + step, m.pt[1]); tick(m)


def lines(m): return [ln.split(b'\t') for ln in m.fs.get(QUEUE, b'').split(b'\n') if ln]


def watch_tests(ctx):
    """1. The play watch: Last.fm's rules (30 s, half or 4 minutes), once a play, seeks, pauses, repeats, slow ticks, what is never queued."""
    expect = ctx.expect
    m = machine(conf=None)
    tick(m)
    expect(('access', CFG) in m.calls and QUEUE not in m.fs, 'no settings file: off, nothing read but the look for it')
    tick(m)
    expect(m.calls == [], f'off, the next tick: no call at all ({m.calls})')
    for _ in range(O['LF_CFG_TICKS']): tick(m)
    expect(('access', CFG) in m.calls, 'looked for again a minute later')

    m = machine(); track(m, length=200); tick(m)
    run_for(m, 99)
    expect(lines(m) == [], f'99 s of a 200 s track: not yet ({lines(m)})')
    run_for(m, 1)
    expect(lines(m) == [[str(T0).encode(), b'200', b'Radiohead', b'Karma Police', b'OK Computer', b'']],
           f'half of it played: queued once, with the time it started ({lines(m)})')
    run_for(m, 100)
    expect(len(lines(m)) == 1, 'played on to the end: still one line')
    start2 = m.wall
    m.pt = (0, 200); tick(m); run_for(m, 100)
    expect(len(lines(m)) == 2 and lines(m)[1][0] == str(start2).encode(), f'the track again from the start (repeat): a new play, queued ({lines(m)})')

    m = machine(); track(m, length=900, aartist='Various Artists'); tick(m); run_for(m, 239)
    expect(lines(m) == [], '239 s of a 15 min track: not yet')
    run_for(m, 1)
    expect(len(lines(m)) == 1 and lines(m)[0][1] == b'900' and lines(m)[0][5] == b'Various Artists', f'4 minutes of a long track: queued, the album artist too ({lines(m)})')

    m = machine(); track(m, length=29); tick(m); run_for(m, 29)
    expect(lines(m) == [], 'a 29 s track played through: never (under 30 s)')

    m = machine(); track(m, length=200); tick(m); run_for(m, 50)
    m.status = 3
    for _ in range(100): m.wall += 1; tick(m)
    m.status = 2; run_for(m, 49)
    expect(lines(m) == [], 'paused for 100 s between 50 s and 99 s of play: not yet')
    run_for(m, 1)
    expect(len(lines(m)) == 1 and lines(m)[0][0] == str(T0).encode(), 'the 100th second played: queued, the start before the pause')

    m = machine(); track(m, length=200); tick(m); run_for(m, 10)
    m.wall += 1; m.pt = (150, 200); tick(m); run_for(m, 50)
    expect(lines(m) == [], 'a seek from 10 s to 150 s does not count: 60 s played')

    m = machine(); track(m, length=200); tick(m); run_for(m, 100, step=5)
    expect(len(lines(m)) == 1, 'a tick every 5 s (the screen off): the seconds still count')

    m = machine(); track(m, length=200); tick(m)
    m.play([m.record(title='Other', path='/mnt/mmc/m/2.flac')], 0); m.pt = (0, 200); tick(m); run_for(m, 60)
    m.play([m.record(title='Karma Police', path='/mnt/mmc/m/1.flac')], 0); m.pt = (60, 200); tick(m); run_for(m, 60)
    expect(lines(m) == [], 'another track between: the count starts again')

    for name, setup in (('a podcast episode', lambda m: track(m, path='/mnt/mmc/Podcasts/Show/ep1.mp3')),
                        ('no artist (and no tags of the file)', lambda m: track(m, artist='')),
                        ('nothing playing', lambda m: setattr(m, 'play_pos', -1)),
                        ('no time from the player', lambda m: (track(m), setattr(m, 'pt', None)))):
        m = machine(); setup(m); tick(m)
        for _ in range(120):
            m.wall += 1
            if m.pt: m.pt = (m.pt[0] + 1, m.pt[1])
            tick(m)
        expect(lines(m) == [], f'{name}: never queued')

    m = machine(); m.wall = 1600000000; track(m); tick(m); run_for(m, 100)
    expect(lines(m) == [], 'a clock that was never set (2020): not queued')

    m = machine(); track(m, artist='', title='', album='')
    m.u.mem_write(INFO, b'/mnt/mmc/m/1.flac\0')
    for off, s in ((O['ID3_ARTIST'], 'Björk'), (O['ID3_TITLE'], 'Jóga'), (O['ID3_ALBUM'], 'Homogenic')): m.u.mem_write(INFO + off, s.encode() + b'\0')
    tick(m); run_for(m, 100)
    expect(lines(m) == [[str(T0).encode(), b'200', 'Björk'.encode(), 'Jóga'.encode(), b'Homogenic', b'']], f'a record without tags: the playing file\'s own tags ({lines(m)})')

    m = machine(); track(m, title='A\tB\nC', album='é' * 200); tick(m); run_for(m, 100)
    ln = lines(m)[0]
    expect(ln[3] == b'A B C' and ln[4] == ('é' * 127).encode(), f'a tab or a line break becomes a space; a long tag ends before LF_FIELD bytes, never in a character ({ln[3]!r}, {len(ln[4])})')

    m = machine(); m.fs[QUEUE] = b'x' * O['LF_QCAP']; track(m); tick(m); run_for(m, 100)
    expect(len(m.fs[QUEUE]) == O['LF_QCAP'], 'a full queue: the play is not kept')

    # the battery hook calls the tick, not with the kill switch
    m = machine(conf=None); m.setword(pod.DATA['SYSTEM_BAR'][0], m.alloc(0x80))
    m.call(PSYMS['bp_capacity'], count=400000)
    expect(('access', CFG) in m.calls, 'the status bar tick (bp_capacity) runs the scrobble tick')
    m = machine(conf=None); m.setword(pod.DATA['SYSTEM_BAR'][0], m.alloc(0x80)); m.fs[KILL] = b''
    m.call(PSYMS['bp_capacity'], count=400000)
    expect(('access', CFG) not in m.calls, 'with /mnt/mmc/m1plus-off: not')


def queued(m, n, start=T0):
    """n plays in the queue, as the watch writes them (some without an album, Unicode tags)."""
    rnd = random.Random(n)
    out = b''
    for i in range(n):
        artist = rnd.choice(['Radiohead', 'Sigur Rós', '坂本龍一', 'AC/DC & Friends', 'Röyksopp'])
        album = rnd.choice(['', 'OK Computer', 'Ágætis byrjun', '1+1=2 ?'])
        out += f'{start + 300 * i}\t{rnd.randint(30, 900)}\t{artist}\tSong {i} (live)\t{album}\t{"" if i % 3 else artist}\n'.encode()
    m.fs[QUEUE] = out
    return out


def server(m, body=b'<?xml version="1.0"?>\n<lfm status="ok"><scrobbles accepted="1" ignored="0"></scrobbles></lfm>', rc=0):
    seen = []

    def site(url, o):
        seen.append((url, bytes(m.u.mem_read(o[10015], o[60])), o))
        return {'rc': rc, 'code': 200, 'body': body}
    m.curl_site = site
    return seen


def signed(params):
    """Last.fm's api_sig: the parameters but api_sig sorted by name, name and value each, then the secret, MD5 hex."""
    s = b''.join(k.encode() + v.encode() for k, v in sorted((k, v) for k, v in params.items() if k != 'api_sig'))
    return hashlib.md5(s + SECRET.encode()).hexdigest()


def send(m):
    """One tick that starts the worker, the worker's run, and the tick that takes its answer."""
    if m.thread is None: tick(m)
    th, m.thread = m.thread, None
    if th: m.call(th[0], a0=th[1], count=1 << 40)
    m.wall += 1; tick(m)   # (this tick may start the next request: m.thread)
    return th is not None


def send_tests(ctx):
    """2. The sender: when it posts, the signed request, the queue after each answer, the back-off."""
    expect = ctx.expect
    m = machine(wifi=-1); q = queued(m, 3); tick(m)
    expect(m.thread is None and m.fs[QUEUE] == q, 'plays waiting, no Wi-Fi: no thread')
    m.wifi = 4; m.wall += 10; tick(m)
    expect(m.thread is None, 'the Wi-Fi up: looked at again only after LF_IDLE_LOOK')
    m.wall += O['LF_IDLE_LOOK']; seen = server(m)
    expect(send(m) and len(seen) == 1 and m.fs[QUEUE] == b'', f'then sent, and the queue emptied on "ok" ({m.fs[QUEUE][:40]!r})')
    url, body, o = seen[0]
    p = {k: v[0] for k, v in urllib.parse.parse_qs(body.decode(), keep_blank_values=True, strict_parsing=True).items()}
    expect(url == 'https://ws.audioscrobbler.com/2.0/' and o[10065] == CA and o[181] == 2 and o[99] == 1, 'POST to Last.fm over HTTPS only, the CA bundle, no signals')
    expect(p['method'] == 'track.scrobble' and p['api_key'] == KEY and p['sk'] == SK and p['api_sig'] == signed(p), f'signed with the secret: api_sig matches Python\'s MD5 ({p["api_sig"]})')
    rows = [ln.split('\t') for ln in q.decode().splitlines()]
    expect(all(p[f'timestamp[{i}]'] == r[0] and p[f'duration[{i}]'] == r[1] and p[f'artist[{i}]'] == r[2] and p[f'track[{i}]'] == r[3]
               and p.get(f'album[{i}]', '') == r[4] and p.get(f'albumArtist[{i}]', '') == r[5] for i, r in enumerate(rows)),
           'each play\'s fields, indexed; an empty album or album artist not sent')

    # the signature at every message length around MD5's block edges, and 50 plays
    for n in (1, 2, 4, 7, 50):
        for extra in range(0, 70, 7):
            m = machine(wifi=4); q = queued(m, n); m.fs[QUEUE] = q.replace(b'Song 0 ', b'Song 0 ' + b'x' * extra); seen = server(m)
            send(m)
            p = {k: v[0] for k, v in urllib.parse.parse_qs(seen[0][1].decode(), keep_blank_values=True).items()}
            if p['api_sig'] != signed(p): break
        expect(p['api_sig'] == signed(p), f'{n} plays: the signature right at every length tried')

    m = machine(wifi=4); q = queued(m, 60); seen = server(m); send(m)
    p = {k: v[0] for k, v in urllib.parse.parse_qs(seen[0][1].decode()).items()}
    expect('track[49]' in p and 'track[50]' not in p and len(lines(m)) == 10, f'60 plays: 50 a request, 10 left ({len(lines(m))})')
    send(m)
    expect(len(seen) == 2 and m.fs[QUEUE] == b'', 'the rest at the next tick')

    m = machine(wifi=4); q = queued(m, 2); seen = server(m); tick(m)
    m.fs[QUEUE] += b'1800009999\t200\tNew\tPlay\t\t\n'
    th, m.thread = m.thread, None
    m.call(th[0], a0=th[1], count=1 << 40); m.wall += 1; tick(m)
    expect(m.fs[QUEUE] == b'1800009999\t200\tNew\tPlay\t\t\n', 'a play queued while the request ran stays')

    m = machine(wifi=4); q = queued(m, 2); m.fs[QUEUE] = b'garbage line\n' + q; seen = server(m); send(m)
    p = {k: v[0] for k, v in urllib.parse.parse_qs(seen[0][1].decode()).items()}
    expect('track[1]' in p and 'track[2]' not in p and m.fs[QUEUE] == b'', 'a broken line is left out of the request and leaves the queue with the batch')

    # the answers
    m = machine(wifi=4); q = queued(m, 2); seen = server(m, rc=7)
    send(m)
    expect(m.fs[QUEUE] == q, 'no connection: the plays stay')
    m.wall += 58; send(m)
    expect(len(seen) == 1, 'not again within LF_RETRY_MIN')
    m.wall += 2; send(m)
    expect(len(seen) == 2, 'again after a minute')
    m.wall += 119; send(m)
    expect(len(seen) == 2, 'then the wait doubles')
    m.wall += 2; server(m); send(m)
    expect(m.fs[QUEUE] == b'', 'and the plays go once Last.fm answers')

    for code, kept, what in ((9, True, 'an invalid session'), (10, True, 'an invalid key'), (11, True, 'the service offline'), (29, True, 'rate limited'), (6, False, 'invalid parameters')):
        m = machine(wifi=4); q = queued(m, 2)
        seen = server(m, body=f'<?xml version="1.0"?>\n<lfm status="failed"><error code="{code}">x</error></lfm>'.encode())
        send(m)
        expect((m.fs[QUEUE] == q) == kept, f'error {code} ({what}): the plays {"stay" if kept else "are dropped"}')
    m = machine(wifi=4); q = queued(m, 2); seen = server(m, body=b'<lfm status="failed"><error code="9">Invalid session key</error></lfm>'); send(m)
    m.wall += 1800; send(m)
    expect(len(seen) == 1, 'a refused session: no request for a while')
    m.fs[CFG] = CONF.replace(SK.encode(), b'b' * 32); server(m)
    for _ in range(O['LF_CFG_TICKS']): m.wall += 1; tick(m)
    send(m)
    expect(m.fs[QUEUE] == b'', 'a new login on the card: sent at once')

    m = machine(wifi=4); q = queued(m, 2); m.thread_ok = False; seen = server(m); tick(m)
    expect(m.fs[QUEUE] == q and not seen, 'no thread: nothing lost, tried later')


SECTIONS = [watch_tests, send_tests]
