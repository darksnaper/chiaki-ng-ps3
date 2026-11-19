#!/usr/bin/env python3
"""Sign in to PSN on your Mac/PC and generate a chiaki.cfg for Chiaki-ng PS3.

Zero external dependencies (Python 3 standard library only).
No configuration is required on the PS5 side (other than Remote Play being enabled
and the PS5 being signed in to your PSN account).

Usage:
    python3 ps3/tools/psn_login.py [output_file]
"""

import base64
import json
import struct
import sys
import urllib.error
import urllib.parse
import urllib.request
import webbrowser

CLIENT_ID = "ba495a24-818c-472b-b12d-ff231c1b5745"
CLIENT_SECRET = "mvaiZkRsAsI1IBkY"
REDIRECT_URI = "https://remoteplay.dl.playstation.net/remoteplay/redirect"
SCOPE = (
    "psn:clientapp "
    "referenceDataService:countryConfig.read "
    "pushNotification:webSocket.desktop.connect "
    "sessionManager:remotePlaySession.system.update"
)
LOGIN_URL = (
    "https://auth.api.sonyentertainmentnetwork.com/2.0/oauth/authorize?"
    + urllib.parse.urlencode(
        {
            "service_entity": "urn:service-entity:psn",
            "response_type": "code",
            "client_id": CLIENT_ID,
            "redirect_uri": REDIRECT_URI,
            "scope": SCOPE,
            "request_locale": "en_US",
            "ui": "pr",
            "service_logo": "ps",
            "layout_type": "popup",
            "smcid": "remoteplay",
            "prompt": "always",
            "PlatformPrivacyWs1": "minimal",
        }
    )
)
TOKEN_URL = "https://auth.api.sonyentertainmentnetwork.com/2.0/oauth/token"
USER_ID_URL = "https://auth.api.sonyentertainmentnetwork.com/2.0/oauth/token/"
DEVICES_URL = (
    "https://web.np.playstation.com/api/cloudAssistedNavigation/v2/users/me/clients"
    "?platform=PS5&includeFields=device&limit=10&offset=0"
)


def exchange_code(code: str) -> dict:
    basic = base64.b64encode(f"{CLIENT_ID}:{CLIENT_SECRET}".encode("ascii")).decode("ascii")
    body = (
        f"grant_type=authorization_code&code={urllib.parse.quote(code)}"
        f"&scope={urllib.parse.quote(SCOPE)}"
        f"&redirect_uri={REDIRECT_URI}&"
    ).encode("utf-8")
    req = urllib.request.Request(
        TOKEN_URL,
        data=body,
        headers={
            "Authorization": f"Basic {basic}",
            "Content-Type": "application/x-www-form-urlencoded",
        },
        method="POST",
    )
    with urllib.request.urlopen(req, timeout=20) as resp:
        return json.loads(resp.read().decode("utf-8"))


def fetch_account_id(access_token: str) -> str:
    req = urllib.request.Request(
        USER_ID_URL + urllib.parse.quote(access_token),
        headers={"Authorization": f"Bearer {access_token}"},
        method="GET",
    )
    with urllib.request.urlopen(req, timeout=20) as resp:
        data = json.loads(resp.read().decode("utf-8"))
    user_id = int(data["user_id"])
    return base64.b64encode(struct.pack("<Q", user_id)).decode("ascii")


def fetch_ps5_devices(access_token: str) -> list:
    req = urllib.request.Request(
        DEVICES_URL,
        headers={
            "Authorization": f"Bearer {access_token}",
            "Accept-Language": "en",
        },
        method="GET",
    )
    with urllib.request.urlopen(req, timeout=20) as resp:
        data = json.loads(resp.read().decode("utf-8"))
    out = []
    for c in data.get("clients", []):
        duid = c.get("duid", "")
        dev = c.get("device", {})
        name = dev.get("name", "PS5")
        rp_enabled = "remotePlay" in dev.get("enabledFeatures", [])
        if duid:
            out.append({"name": name, "duid": duid, "rp_enabled": rp_enabled})
    return out


def extract_code(raw: str) -> str:
    raw = raw.strip()
    if "code=" in raw:
        parsed = urllib.parse.urlparse(raw)
        qs = urllib.parse.parse_qs(parsed.query)
        if "code" in qs and qs["code"]:
            return qs["code"][0]
    return raw


def main():
    out_path = sys.argv[1] if len(sys.argv) > 1 else "chiaki.cfg"
    print("=== Chiaki-ng PS3 — PSN Remote Play Setup ===")
    print()
    print("1. Open this URL in your browser and sign in to your PSN account:")
    print()
    print(LOGIN_URL)
    print()
    try:
        webbrowser.open(LOGIN_URL)
    except Exception:
        pass
    print("2. After signing in, the browser will show a blank page saying 'redirect'.")
    print("   Copy the FULL URL from the browser's address bar and paste it below.")
    print()
    raw = input("Paste redirect URL (or code): ").strip()
    code = extract_code(raw)
    if not code:
        sys.exit("Error: empty authorization code.")

    try:
        token_data = exchange_code(code)
    except urllib.error.HTTPError as e:
        sys.exit(f"Error exchanging code: HTTP {e.code} {e.read().decode('utf-8', 'replace')}")

    access_token = token_data.get("access_token", "")
    refresh_token = token_data.get("refresh_token", "")
    if not access_token:
        sys.exit("Error: Sony did not return an access_token.")

    account_id_b64 = ""
    try:
        account_id_b64 = fetch_account_id(access_token)
        print(f"PSN Account ID (base64): {account_id_b64}")
    except Exception as e:
        print(f"Warning: could not fetch PSN account ID automatically ({e})")

    devices = []
    try:
        devices = fetch_ps5_devices(access_token)
        print(f"Found {len(devices)} PS5 console(s) on your PSN account:")
        for d in devices:
            status = "Remote Play enabled" if d["rp_enabled"] else "Remote Play disabled"
            print(f"  - {d['name']} ({status})  DUID={d['duid']}")
    except Exception as e:
        print(f"Warning: could not list PS5 consoles ({e})")

    lines = [
        "# Chiaki-ng PS3 configuration (PSN Internet Remote Play)",
        "resolution=720",
        "fps=60",
        "pacing=1",
        "low_latency=1",
        "vibration=1",
    ]
    if account_id_b64:
        lines.append(f"psn_account_id={account_id_b64}")
    if refresh_token:
        lines.append(f"psn_refresh_token={refresh_token}")
    lines.append(f"psn_auth_token={access_token}")

    for d in devices:
        lines.extend(
            [
                "",
                "[host]",
                f"name={d['name']}",
                "host=",
                "ps5=1",
                "use_psn=1",
                f"duid={d['duid']}",
            ]
        )

    with open(out_path, "w", encoding="utf-8") as f:
        f.write("\n".join(lines) + "\n")

    print()
    print(f"Wrote {out_path}!")
    print("Copy chiaki.cfg to the root of a USB stick (or /dev_hdd0/game/CHIAKIPS3/USRDIR/),")
    print("start Chiaki-ng PS3 on your PS3, and press X on your PS5 to play over the Internet.")


if __name__ == "__main__":
    main()
