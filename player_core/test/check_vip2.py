import json, urllib.request

UIN = "3301168409"
AUTHST = open(r"F:\kpgui\player_core\test\authst.txt", "r", encoding="utf-8").read().strip()
URL = "https://u.y.qq.com/cgi-bin/musicu.fcg"

def call(label, payload, cookie=None, extra_hdr=None):
    hdr = {
        "Content-Type": "application/json",
        "User-Agent": "Mozilla/5.0 (Windows NT 10.0; Win64; x64) AppleWebKit/537.36 Chrome/120.0 Safari/537.36",
        "Referer": "https://y.qq.com/",
    }
    if cookie:
        hdr["Cookie"] = cookie
    if extra_hdr:
        hdr.update(extra_hdr)
    req = urllib.request.Request(URL, data=json.dumps(payload).encode(), headers=hdr)
    print("=" * 58)
    print("[%s]" % label)
    try:
        with urllib.request.urlopen(req, timeout=15) as r:
            j = json.loads(r.read().decode("utf-8", "replace"))
        print(json.dumps(j, ensure_ascii=False)[:1800])
    except Exception as e:
        print("ERROR:", e)

COOKIE = "uin=%s; qqmusic_key=%s; qm_keyst=%s; qqmusic_uin=%s" % (UIN, AUTHST, AUTHST, UIN)

MOD = "music.UserInfo.userInfoServer"

# A) Cookie 鉴权 + ct19/cv1859
call("A: cookie + ct19/cv1859 GetLoginUserInfo",
     {"comm": {"ct": 19, "cv": 1859, "uin": UIN, "format": "json"},
      "req_1": {"module": MOD, "method": "GetLoginUserInfo", "param": {}}},
     cookie=COOKIE)

# B) Cookie 鉴权 + ct24
call("B: cookie + ct24 GetLoginUserInfo",
     {"comm": {"ct": 24, "cv": 0, "uin": UIN},
      "req_1": {"module": MOD, "method": "GetLoginUserInfo", "param": {}}},
     cookie=COOKIE)

# C) comm.authst + tmeLoginType=1
call("C: comm.authst tmeLoginType=1",
     {"comm": {"ct": 19, "cv": 1859, "uin": UIN, "authst": AUTHST, "tmeLoginType": "1"},
      "req_1": {"module": MOD, "method": "GetLoginUserInfo", "param": {}}})

# D) cookie + authst 双管齐下, ct19
call("D: cookie + comm.authst 双保险",
     {"comm": {"ct": 19, "cv": 1859, "uin": UIN, "authst": AUTHST, "tmeLoginType": "1"},
      "req_1": {"module": MOD, "method": "GetLoginUserInfo", "param": {}}},
     cookie=COOKIE)

# E) 试试 GetVipInfo via cookie
call("E: cookie GetVipInfo",
     {"comm": {"ct": 19, "cv": 1859, "uin": UIN},
      "req_1": {"module": "music.vipinfo.VipInfoServer", "method": "GetVipInfo",
                "param": {"uin": UIN}}},
     cookie=COOKIE)
