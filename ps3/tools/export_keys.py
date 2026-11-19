#!/usr/bin/env python3
"""Turn a chiaki-ng settings export into a chiaki.cfg for the PS3 port.

In chiaki-ng on your PC: Settings -> General -> Export settings (saves an .ini).
Then:  python3 export_keys.py exported.ini [PS4 IP address] > chiaki.cfg

Without an IP address the PS3 searches the network for the PS4 (matched by its MAC).
All registered PS4s and PS5s are exported, plus the PSN account id (for registering more consoles
from the PS3 with a PIN).
"""
import re
import sys


def qt_unescape(value):
    """Decode a value as written by QSettings in IniFormat."""
    value = value.strip()
    out = []
    in_quotes = False
    i = 0
    simple = {'a': 7, 'b': 8, 'f': 12, 'n': 10, 'r': 13, 't': 9, 'v': 11,
              '\\': 92, '"': 34, "'": 39, '?': 63, ';': 59, ',': 44}
    while i < len(value):
        c = value[i]
        if c == '"':
            in_quotes = not in_quotes
            i += 1
        elif c == '\\' and i + 1 < len(value):
            n = value[i + 1]
            if n in simple:
                out.append(simple[n]); i += 2
            elif n == 'x':
                j = i + 2
                while j < len(value) and value[j] in '0123456789abcdefABCDEF':
                    j += 1
                out.append(int(value[i + 2:j], 16) & 0xff); i = j
            elif n in '01234567':
                j = i + 1
                while j < len(value) and j < i + 4 and value[j] in '01234567':
                    j += 1
                out.append(int(value[i + 1:j], 8) & 0xff); i = j
            else:
                out.append(ord(n) & 0xff); i += 2
        else:
            out.extend(c.encode('utf-8')); i += 1
    return bytes(out)


def byte_array(raw):
    data = qt_unescape(raw)
    if data.startswith(b'@ByteArray(') and data.endswith(b')'):
        return data[len('@ByteArray('):-1]
    return data


def main():
    if len(sys.argv) not in (2, 3):
        sys.exit(__doc__)
    path = sys.argv[1]
    host = sys.argv[2] if len(sys.argv) == 3 else ''
    hosts = {}
    settings = {}
    section = None
    with open(path, encoding='utf-8', errors='surrogateescape') as f:
        for line in f:
            line = line.rstrip('\r\n')
            if line.startswith('['):
                section = line.strip('[]')
                continue
            if '=' not in line:
                continue
            key, val = line.split('=', 1)
            if section == 'settings':
                settings[key.strip()] = val
                continue
            if section != 'registered_hosts':
                continue
            m = re.match(r'(\d+)\\(\w+)$', key.strip())
            if m:
                hosts.setdefault(int(m.group(1)), {})[m.group(2)] = val
    consoles = [h for _, h in sorted(hosts.items())]
    if not consoles:
        sys.exit('No registered console found in ' + path)

    print('# Chiaki PS3 configuration, exported from chiaki-ng')
    print('resolution=720')
    print('fps=60')
    account = qt_unescape(settings.get('psn_account_id', '')).decode('ascii', 'replace').strip()
    if account:
        print('# PSN account id: lets the PS3 register consoles and connect over PSN')
        print('psn_account_id=%s' % account)
    refresh_token = qt_unescape(settings.get('psn_refresh_token', '')).decode('ascii', 'replace').strip()
    if refresh_token:
        print('# PSN OAuth2 refresh token: lets the PS3 wake & stream your remote PS5 over the Internet')
        print('psn_refresh_token=%s' % refresh_token)
    auth_token = qt_unescape(settings.get('psn_auth_token', '')).decode('ascii', 'replace').strip()
    if auth_token:
        print('psn_auth_token=%s' % auth_token)
    for i, h in enumerate(consoles):
        name = qt_unescape(h.get('server_nickname', '')).decode('utf-8', 'replace')
        regist_key = byte_array(h['rp_regist_key'])
        rp_key = byte_array(h['rp_key'])
        if len(regist_key) != 16 or len(rp_key) != 16:
            sys.exit('Unexpected key length for %s (%d/%d bytes)' % (name, len(regist_key), len(rp_key)))
        mac = byte_array(h['server_mac']).hex().upper() if 'server_mac' in h else ''
        print()
        print('[host]')
        if name:
            print('name=%s' % name)
        # The IP address applies to the first console only; empty = found on the network by host_id.
        print('host=%s' % (host if i == 0 else ''))
        if len(mac) == 12:
            print('host_id=%s' % mac)
        print('ps5=%d' % (1 if int(h.get('target', '0')) >= 1000000 else 0))
        print('regist_key=%s' % regist_key.hex())
        print('rp_key=%s' % rp_key.hex())


if __name__ == '__main__':
    main()
