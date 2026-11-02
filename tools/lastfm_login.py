#!/usr/bin/env python3
"""Sign the player in to Last.fm (patch/scrobble.c, docs/modules/scrobble.md): run on the computer with the player's card mounted.

  make lastfm VOLUME=/run/media/$USER/SHANLING        sign in: asks for your API key and secret, user name and password
  make lastfm VOLUME=... OFF=1                         sign out: remove the file, scrobbling stops

Your own API account (https://www.last.fm/api/account/create) gives the key and the secret. The password goes once to Last.fm's auth.getMobileSession over
HTTPS and is never written anywhere; the answer, a session key that does not expire, is written with the key and the secret to <card>/.m1plus/lastfm.
Plays queued on the card before (<card>/.m1plus/scrobbles) are sent with the new session.
"""
import argparse, getpass, hashlib, json, os, sys, urllib.error, urllib.parse, urllib.request

API = 'https://ws.audioscrobbler.com/2.0/'


def sign(params, secret):
    return hashlib.md5((''.join(k + params[k] for k in sorted(params)) + secret).encode()).hexdigest()


def session(key, secret, user, password):
    params = {'method': 'auth.getMobileSession', 'api_key': key, 'username': user, 'password': password}
    params['api_sig'] = sign(params, secret)
    data = urllib.parse.urlencode({**params, 'format': 'json'}).encode()
    try:
        with urllib.request.urlopen(urllib.request.Request(API, data=data, headers={'User-Agent': 'M1PlusMusic/1.0'}), timeout=30) as r:
            answer = json.load(r)
    except urllib.error.HTTPError as e:
        answer = json.load(e)
    if 'session' not in answer:
        sys.exit(f'Last.fm refused the sign-in: {answer.get("message", answer)}')
    return answer['session']['key'], answer['session']['name']


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('--volume', required=True, help="the player's card, mounted")
    ap.add_argument('--off', action='store_true', help='remove the sign-in (scrobbling stops; queued plays stay on the card)')
    a = ap.parse_args()
    card = os.path.join(a.volume, '.m1plus')
    path = os.path.join(card, 'lastfm')
    if not os.path.isdir(a.volume) or not os.path.ismount(a.volume):
        sys.exit(f'{a.volume} is not a mounted card')
    if a.off:
        if os.path.exists(path): os.remove(path)
        print(f'Removed {path}: the player stops scrobbling within a minute.')
        return
    key = input('Last.fm API key: ').strip()
    secret = getpass.getpass('Last.fm shared secret: ').strip()
    user = input('Last.fm user name: ').strip()
    password = getpass.getpass('Last.fm password: ')
    for name, v in (('API key', key), ('shared secret', secret)):
        if len(v) != 32 or any(c not in '0123456789abcdef' for c in v.lower()):
            sys.exit(f'The {name} should be 32 hexadecimal digits')
    sk, name = session(key, secret, user, password)
    os.makedirs(card, exist_ok=True)
    tmp = path + '.tmp'
    with open(tmp, 'w') as f:
        f.write(f'api_key={key}\nsecret={secret}\nsk={sk}\n')
        f.flush(); os.fsync(f.fileno())
    os.replace(tmp, path)
    print(f'Signed in as {name}. Wrote {path}; eject the card and the player scrobbles from now on.')


if __name__ == '__main__': main()
