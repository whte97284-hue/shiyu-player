import json, urllib.request

UIN = "3301168409"
AUTHST = open(r"F:\kpgui\player_core\test\authst.txt", "r", encoding="utf-8").read().strip()
URL = "https://u.y.qq.com/cgi-bin/musicu.fcg"

def post(label, payload, cookie=None):
    hdr = {"Content-Type": "application/json",
           "User-Agent": "Mozilla/5.0 (Windows NT 10.0; Win64; x64) Chrome/120.0",
           "Referer": "https://y.qq.com/"}
    if cookie:
        hdr["Cookie"] = cookie
    req = urllib.request.Request(URL, data=json.dumps(payload).encode(), headers=hdr)
    print("=" * 58)
    print("[%s]" % label)
    try:
        with urllib.request.urlopen(req, timeout=15) as r:
            j = json.loads(r.read().decode("utf-8", "replace"))
        print(json.dumps(j, ensure_ascii=False)[:1500])
        return j
    except Exception as e:
        print("ERROR:", e)
        return None

comm_auth = {"ct": 19, "cv": 1859, "uin": UIN, "authst": AUTHST, "tmeLoginType": "1",
             "format": "json"}

# 校准 1：不存在的模块 → 看错误码长什么样
post("CAL-1: fake module",
     {"comm": comm_auth, "req_1": {"module": "music.fake.NoSuchModule",
                                   "method": "NoSuchMethod", "param": {}}})

# 校准 2：GetVkey 免费歌（只需登录，不需 VIP）→ 验证登录态
post("CAL-2: GetVkey (login check)",
     {"comm": comm_auth,
      "req_1": {"module": "music.vkey.GetVkeyServer", "method": "CgiGetVkey",
                "param": {"filename": ["M500000uSajZ0lnlEE.mp3"],
                          "guid": "10000",
                          "songmid": ["000uSajZ0lnlEE"],
                          "songtype": [0], "uin": UIN, "loginflag": 1,
                          "platform": "27"}}})

# 校准 3：GetEVkey → 验证 VIP
post("CAL-3: GetEVkey (VIP check)",
     {"comm": comm_auth,
      "req_1": {"module": "music.vkey.GetEVkey", "method": "CgiGetEVkey",
                "param": {"filename": ["M500000uSajZ0lnlEE.mp3"],
                          "guid": "10000",
                          "songmid": ["000uSajZ0lnlEE"],
                          "songtype": [1], "uin": UIN, "loginflag": 1,
                          "platform": "27", "ctx": 1}}})
