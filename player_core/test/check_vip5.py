import json, urllib.request

UIN = "3301168409"
AUTHST = open(r"F:\kpgui\player_core\test\authst.txt", "r", encoding="utf-8").read().strip()
URL = "https://u.y.qq.com/cgi-bin/musicu.fcg"

def post(label, payload, cookie=None, quiet=False):
    hdr = {"Content-Type": "application/json",
           "User-Agent": "Mozilla/5.0 (Windows NT 10.0; Win64; x64) Chrome/120.0",
           "Referer": "https://y.qq.com/"}
    if cookie: hdr["Cookie"] = cookie
    req = urllib.request.Request(URL, data=json.dumps(payload).encode(), headers=hdr)
    print("=" * 58); print("[%s]" % label)
    try:
        with urllib.request.urlopen(req, timeout=15) as r:
            j = json.loads(r.read().decode("utf-8", "replace"))
        if not quiet:
            print(json.dumps(j, ensure_ascii=False)[:1200])
        return j
    except Exception as e:
        print("ERROR:", e); return None

# ── 1) 取官方歌单，正确抽取 mid / media_mid
print("### STEP 1: 取歌单曲目 ###")
j = post("diss info", {"comm": {"ct": 24, "cv": 0, "format": "json"},
     "req_1": {"module": "music.srfDissInfo.aiDissInfo", "method": "uniform_get_Dissinfo",
               "param": {"disstid": 8668894184, "song_begin": 0, "song_num": 5}}}, quiet=True)
songs = []
try:
    sl = j["req_1"]["data"]["songlist"]
    for s in sl[:5]:
        mid = s.get("mid") or s.get("songmid")
        f = s.get("file", {}) or {}
        mmid = f.get("media_mid")
        # mid 可能是 base64 编码的，优先取 "mid" 字段
        print("  title=%s | mid=%s | media_mid=%s | size128=%s size320=%s sizeflac=%s" % (
            s.get("name") or s.get("title"), mid, mmid,
            f.get("size_128mp3"), f.get("size_320mp3"), f.get("size_flac")))
        if mid: songs.append((mid, mmid))
except Exception as e:
    print("parse err:", e, list(j.get("req_1", {}).keys()))

if not songs:
    print("!! 没拿到歌，退出"); raise SystemExit

mid0, mmid0 = songs[0]
print("\n使用: mid=%s media_mid=%s" % (mid0, mmid0))

comm = {"ct": 19, "cv": 1859, "uin": UIN, "authst": AUTHST, "tmeLoginType": "1", "format": "json"}
comm_bad = dict(comm); comm_bad["authst"] = "FAKE_INVALID_AUTHST_000"

def vkey(commx, filename, songtype, mod, meth, tag):
    return post(tag, {"comm": commx,
        "req_1": {"module": mod, "method": meth,
                  "param": {"filename": [filename], "guid": "10000",
                            "songmid": [mid0], "songtype": [songtype],
                            "uin": UIN, "loginflag": 1, "platform": "27", "ctx": 1}}})

# ── 2) GetVkey：免费歌能拿到 purl → 验证登录态
print("\n### STEP 2: GetVkey（验证登录态）###")
vkey(comm, "M500%s.mp3" % mid0, 0, "music.vkey.GetVkey", "CgiGetVkey", "Vkey 真auth")
vkey(comm_bad, "M500%s.mp3" % mid0, 0, "music.vkey.GetVkey", "CgiGetVkey", "Vkey 假auth(A/B)")

# ── 3) GetEVkey：ekey 有值 → 有 VIP
print("\n### STEP 3: GetEVkey（验证 VIP）###")
for fn in ["M500%s.mgg" % mid0, "M800%s.mgg" % mid0, "F000%s.mflac" % mid0]:
    vkey(comm, fn, 1, "music.vkey.GetEVkey", "CgiGetEVkey", "EVkey 真auth %s" % fn)
vkey(comm_bad, "M500%s.mgg" % mid0, 1, "music.vkey.GetEVkey", "CgiGetEVkey", "EVkey 假auth(A/B)")
