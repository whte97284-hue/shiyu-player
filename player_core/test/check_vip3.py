import json, urllib.request, urllib.parse

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
        print(json.dumps(j, ensure_ascii=False)[:1600])
        return j
    except Exception as e:
        print("ERROR:", e)
        return None

# ── 0) 验证管道：匿名歌单模块（我们之前实测过能通）
post("0: pipe check - anonymous diss info",
     {"comm": {"ct": 24, "cv": 0, "format": "json"},
      "req_1": {"module": "music.srfDissInfo.aiDissInfo",
                "method": "uniform_get_Dissinfo",
                "param": {"disstid": 8668894184, "enc_host_uin": "", "tag": 1,
                          "userinfo": 1, "song_begin": 0, "song_num": 1}}})

# ── 1) 拿一首歌的 mid（匿名搜索）
print("=" * 58)
print("[1: search song]")
try:
    q = urllib.parse.urlencode({"p": 1, "n": 3, "w": "周杰伦 晴天", "format": "json",
                                "aggr": 0, "cr": 1, "new_json": 1})
    req = urllib.request.Request("https://c.y.qq.com/soso/fcgi-bin/client_search_cp?" + q,
                                 headers={"User-Agent": "Mozilla/5.0", "Referer": "https://y.qq.com/"})
    with urllib.request.urlopen(req, timeout=15) as r:
        s = json.loads(r.read().decode("utf-8", "replace"))
    lst = s.get("data", {}).get("song", {}).get("list", [])
    for it in lst[:3]:
        print("  mid=%s  media_mid=%s  name=%s  pay=%s" % (
            it.get("songmid"), it.get("file", {}).get("media_mid"),
            it.get("songname"), it.get("pay")))
        if it.get("file", {}).get("media_mid"):
            globals()["SMID"] = it["file"]["media_mid"]
            globals()["SID"] = it.get("songid")
except Exception as e:
    print("ERROR:", e)
