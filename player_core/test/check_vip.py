import json, urllib.request, sys

UIN = "3301168409"
AUTHST = open(r"F:\kpgui\player_core\test\authst.txt", "r", encoding="utf-8").read().strip()

def call(payload, label):
    url = "https://u.y.qq.com/cgi-bin/musicu.fcg"
    data = json.dumps(payload, ensure_ascii=False).encode("utf-8")
    req = urllib.request.Request(url, data=data, headers={
        "Content-Type": "application/json",
        "User-Agent": "Mozilla/5.0 (Windows NT 10.0; Win64; x64) QQMusic/19.51",
        "Referer": "https://y.qq.com/",
    })
    print("=" * 60)
    print("[%s]" % label)
    try:
        with urllib.request.urlopen(req, timeout=15) as r:
            body = r.read().decode("utf-8", "replace")
        try:
            j = json.loads(body)
            print(json.dumps(j, ensure_ascii=False, indent=2)[:2500])
        except Exception:
            print(body[:1500])
    except Exception as e:
        print("ERROR:", e)

comm_full = {"ct": "24", "cv": "0", "uin": UIN, "authst": AUTHST, "tmeLoginType": "3"}
comm_anon = {"ct": "24", "cv": "0", "uin": "0", "format": "json"}

# 1) 登录用户信息（带 authst）
call({"comm": comm_full, "req_1": {"module": "music.UserInfo.userInfoServer",
      "method": "GetLoginUserInfo", "param": {}}}, "GetLoginUserInfo (auth)")

# 2) VIP 信息
call({"comm": comm_full, "req_1": {"module": "music.vipinfo.VipInfoServer",
      "method": "GetVipInfo", "param": {"uin": UIN}}}, "GetVipInfo")

# 3) 另一个常见 VIP 模块
call({"comm": comm_full, "req_1": {"module": "music.UserInfo.userInfoServer",
      "method": "GetVipInfo", "param": {}}}, "GetVipInfo(v2)")

# 4) 用户详情
call({"comm": comm_full, "req_1": {"module": "music.UserInfo.userInfoServer",
      "method": "GetUserDetail", "param": {}}}, "GetUserDetail")
