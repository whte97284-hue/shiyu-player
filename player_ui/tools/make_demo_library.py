#!/usr/bin/env python3
"""
生成界面验证用的演示曲库:16 张虚构专辑,程序化生成风格各异的封面 (检验取色与排版),
中 / 日 / 英文标签,覆盖 24/96、24/192、16/44 FLAC、MP3、多碟、无封面等情况。
用法: python3 make_demo_library.py <输出目录>
依赖: ffmpeg, pip install mutagen pillow
"""
import io
import math
import os
import random
import subprocess
import sys

from PIL import Image, ImageDraw, ImageFilter
from mutagen.flac import FLAC, Picture
from mutagen.id3 import APIC, ID3, TALB, TIT2, TPE1, TRCK

OUT = sys.argv[1] if len(sys.argv) > 1 else "demo_library"
random.seed(7)


def cover(style, palette, size=900):
    a, b, c = [tuple(int(palette[i][k:k + 2], 16) for k in (1, 3, 5)) for i in range(3)]
    im = Image.new("RGB", (size, size), a)
    d = ImageDraw.Draw(im)
    if style == "gradient":
        for y in range(size):
            t = y / size
            d.line([(0, y), (size, y)], fill=tuple(int(a[i] * (1 - t) + b[i] * t) for i in range(3)))
        d.ellipse([size * 0.18, size * 0.2, size * 0.82, size * 0.84], fill=c)
    elif style == "sun":
        for r in range(size, 0, -6):
            t = r / size
            col = tuple(int(b[i] * t + a[i] * (1 - t)) for i in range(3))
            d.ellipse([size / 2 - r / 2, size * 0.62 - r / 2, size / 2 + r / 2, size * 0.62 + r / 2], fill=col)
        d.rectangle([0, size * 0.66, size, size], fill=c)
    elif style == "stripes":
        for i in range(0, size, 36):
            d.rectangle([i, 0, i + 18, size], fill=b if (i // 36) % 2 else c)
        im = im.rotate(18, expand=False, fillcolor=a)
    elif style == "bokeh":
        for _ in range(60):
            r = random.randint(20, 140)
            x, y = random.randint(0, size), random.randint(0, size)
            d.ellipse([x - r, y - r, x + r, y + r], fill=random.choice([b, c]))
        im = im.filter(ImageFilter.GaussianBlur(18))
    elif style == "grid":
        n = 6
        s = size / n
        for i in range(n):
            for j in range(n):
                col = [a, b, c][(i * 3 + j * 5) % 3]
                d.rounded_rectangle([i * s + 8, j * s + 8, (i + 1) * s - 8, (j + 1) * s - 8], radius=18, fill=col)
    elif style == "wave":
        for k in range(14):
            pts = [(x, size * (0.15 + k * 0.06) + 40 * math.sin(x / 70 + k)) for x in range(0, size + 10, 10)]
            d.line(pts, fill=b if k % 2 else c, width=14)
    elif style == "mono":
        d.rectangle([0, 0, size, size], fill=a)
        d.ellipse([size * 0.3, size * 0.3, size * 0.7, size * 0.7], outline=c, width=10)
    b2 = io.BytesIO()
    im.save(b2, format="JPEG", quality=90)
    return b2.getvalue()


ALBUMS = [
    ("夜航星", "林间信号", "gradient", ["#0c1633", "#3a2a6b", "#f2b84b"], 96000, 24, 8),
    ("Neon Tide", "Glass Harbor", "stripes", ["#120a1f", "#ff3d81", "#2de2e6"], 44100, 16, 10),
    ("雨后初霁", "陈奕迅", "bokeh", ["#1d2b2a", "#6fbf9f", "#d9f0e3"], 192000, 24, 6),
    ("北方以北", "苏木", "sun", ["#2b1a12", "#e2683c", "#311e17"], 48000, 24, 9),
    ("アダマス", "LiSA", "grid", ["#1a1a1a", "#c21f3a", "#f4f1ea"], 96000, 24, 5),
    ("Paper Planes", "The Quiet Year", "wave", ["#f1e9da", "#2e4a62", "#c96f4a"], 44100, 16, 12),
    ("深海回声", "蓝鲸计划", "gradient", ["#03141f", "#0b4f6c", "#7fd1f0"], 96000, 24, 7),
    ("Monochrome Days", "Aster", "mono", ["#e9e6df", "#e9e6df", "#121212"], 44100, 16, 8),
    ("晚风与诗", "周深", "bokeh", ["#2a1630", "#d77fa1", "#f6d5c3"], 44100, 24, 10),
    ("Solar Drift", "Kites & Cables", "sun", ["#1b0f2e", "#ff9e3d", "#3b1d4f"], 192000, 24, 6),
    ("森林物语", "久石让", "grid", ["#0f2416", "#3f7d4e", "#d8c78f"], 96000, 24, 11),
    ("Late Night Jazz Trio", "Blue Room", "stripes", ["#0d0f1a", "#2b59c3", "#e6c35c"], 44100, 16, 7),
    ("城市旧梦", "王菲", "wave", ["#1c1410", "#b2643c", "#e8cfa8"], 44100, 16, 9),
    ("Ultraviolet", "Mira Sol", "gradient", ["#14002a", "#7b2ff7", "#f107a3"], 48000, 24, 8),
    ("无封面合集", "群星", None, None, 44100, 16, 6),
    ("Morning Coffee (MP3)", "Daylight Club", "bokeh", ["#2a1c12", "#c08a52", "#f3e3c9"], 44100, 0, 6),
]

TITLES_CN = ["序章", "微光", "风停了", "远行", "雾中人", "旧时光", "回声", "向晚", "星河", "归途", "梦的边缘", "尾声"]
TITLES_EN = ["Intro", "Afterglow", "Paper Moon", "Static", "Gravity", "Sundown", "Echoes", "Night Drive",
             "Halcyon", "Midnight Rain", "Outro", "Lanterns"]
TITLES_JP = ["プロローグ", "アダマス", "シルシ", "Rising Hope", "crossing field"]


def sine(path, secs, rate, bits, mp3=False):
    args = ["ffmpeg", "-v", "error", "-y", "-f", "lavfi", "-i",
            f"sine=frequency={random.choice([220, 330, 440, 523, 659])}:duration={secs}:sample_rate={rate}",
            "-ac", "2", "-map_metadata", "-1"]
    if mp3:
        args += ["-c:a", "libmp3lame", "-b:a", "320k"]
    elif bits == 24:
        args += ["-c:a", "flac", "-sample_fmt", "s32", "-bits_per_raw_sample", "24"]
    else:
        args += ["-c:a", "flac"]
    subprocess.run(args + [path], check=True)


os.makedirs(OUT, exist_ok=True)
for ai, (album, artist, style, pal, rate, bits, n) in enumerate(ALBUMS):
    d = os.path.join(OUT, f"{artist} - {album}")
    os.makedirs(d, exist_ok=True)
    art = cover(style, pal) if style else None
    titles = TITLES_JP if "LiSA" in artist else (TITLES_CN if any("一" <= ch <= "鿿" for ch in album) else TITLES_EN)
    mp3 = bits == 0
    for t in range(n):
        disc = 2 if (album == "森林物语" and t >= 6) else 1
        sub = os.path.join(d, f"CD{disc}") if album == "森林物语" else d
        os.makedirs(sub, exist_ok=True)
        title = titles[t % len(titles)]
        ext = "mp3" if mp3 else "flac"
        path = os.path.join(sub, f"{t + 1:02d}. {title}.{ext}")
        sine(path, 3 + (t % 4), rate, bits, mp3)
        if mp3:
            tag = ID3()
            tag.add(TIT2(encoding=3, text=title))
            tag.add(TPE1(encoding=3, text=artist))
            tag.add(TALB(encoding=3, text=album))
            tag.add(TRCK(encoding=3, text=str(t + 1)))
            if art:
                tag.add(APIC(encoding=3, mime="image/jpeg", type=3, desc="", data=art))
            tag.save(path, v2_version=3)
        else:
            f = FLAC(path)
            f["TITLE"] = title
            f["ARTIST"] = artist if album != "无封面合集" else random.choice(["陈奕迅", "王菲", "Aster", "苏木"])
            f["ALBUM"] = album
            f["TRACKNUMBER"] = str(t + 1)
            f["DISCNUMBER"] = str(disc)
            f["DATE"] = str(2015 + ai % 10)
            if art and ai % 3 != 1:
                p = Picture()
                p.type, p.mime, p.data = 3, "image/jpeg", art
                f.add_picture(p)
            f.save()
    # 演示歌词 (同名 .lrc;曲目很短,按 0.6 秒一行排布)
    if album in ("森林物语", "アダマス", "晚风与诗"):
        verses = {
            "森林物语": ["风穿过整片森林", "落叶在脚边轻声回应", "我把名字刻在树上", "等你某天恰好经过", "阳光从枝桠间漏下", "像一封迟到的信",
                         "我们曾在这里迷路", "也在这里找到彼此", "雨后的泥土很香", "鸟群掠过湖面"],
            "アダマス": ["Shine bright, my adamas", "暗闇を切り裂いて", "この手で掴む未来", "誰にも砕けない光", "叫べ 心のままに",
                       "響け 夜明けの空へ", "Never give up tonight", "輝きを抱いて"],
            "晚风与诗": ["晚风吹过旧街口", "路灯一盏一盏亮了", "你说写诗太慢", "可我只想慢慢写你", "云在天边走散", "又在你眼里重逢",
                       "把今天折进信封", "寄给明天的我们"],
        }[album]
        for t in range(n):
            disc = 2 if (album == "森林物语" and t >= 6) else 1
            sub = os.path.join(d, f"CD{disc}") if album == "森林物语" else d
            title = titles[t % len(titles)]
            lines = [f"[ti:{title}]", f"[ar:{artist}]"]
            for k in range(10):
                ms = int(k * 600)
                lines.append(f"[{ms // 60000:02d}:{(ms // 1000) % 60:02d}.{(ms % 1000) // 10:02d}]{verses[(t + k) % len(verses)]}")
            enc = "gbk" if album == "晚风与诗" else "utf-8"  # 一张专辑用 GBK,验证编码识别
            open(os.path.join(sub, f"{t + 1:02d}. {title}.lrc"), "w", encoding=enc).write("\n".join(lines) + "\n")

    if art and ai % 3 == 1:  # 部分专辑只有目录封面 cover.jpg
        open(os.path.join(d, "cover.jpg"), "wb").write(art)
# ---------------- 加密样本:真实可解密的 NCM (带元数据与封面) + 一个 KGG (非 Windows 下工具不可用) ----------------
try:
    import base64, json, struct
    from cryptography.hazmat.primitives.ciphers import Cipher, algorithms, modes

    def aes_ecb(key_hex, data):
        e = Cipher(algorithms.AES(bytes.fromhex(key_hex)), modes.ECB()).encryptor()
        return e.update(data) + e.finalize()

    def pkcs7(b):
        n = 16 - len(b) % 16
        return b + bytes([n]) * n

    def make_ncm(path, flac_bytes, title, artist, album, cover_bytes):
        rc4 = b"DemoKey" + os.urandom(24).hex().encode()
        key_blob = bytes(x ^ 0x64 for x in aes_ecb("687A4852416D736F356B496E62617857", pkcs7(b"neteasecloudmusic" + rc4)))
        meta = json.dumps({"musicName": title, "artist": [[a, 0] for a in artist.split("/")], "album": album,
                           "bitrate": 900000, "duration": 4000, "format": "flac"}, ensure_ascii=False).encode()
        ct = aes_ecb("2331346C6A6B5F215C5D2630553C2728", pkcs7(b"music:" + meta))
        meta_blob = bytes(x ^ 0x63 for x in b"163 key(Don't modify):" + base64.b64encode(ct))
        box, last, ko = list(range(256)), 0, 0
        for i in range(256):
            sw = box[i]
            c = (sw + last + rc4[ko]) & 0xFF
            ko = (ko + 1) % len(rc4)
            box[i], box[c] = box[c], sw
            last = c
        ks = bytes(box[(box[(i + 1) & 0xFF] + box[(box[(i + 1) & 0xFF] + ((i + 1) & 0xFF)) & 0xFF]) & 0xFF] for i in range(256))
        audio = bytes(b ^ ks[i & 0xFF] for i, b in enumerate(flac_bytes))
        data = (b"CTENFDAM\x01\x70" + struct.pack("<I", len(key_blob)) + key_blob + struct.pack("<I", len(meta_blob)) +
                meta_blob + bytes(9) + struct.pack("<I", len(cover_bytes)) + struct.pack("<I", len(cover_bytes)) +
                cover_bytes + audio)
        open(path, "wb").write(data)

    enc_dir = os.path.join(OUT, "网易云下载")
    os.makedirs(enc_dir, exist_ok=True)
    for i, (title, artist) in enumerate([("起风了", "买辣椒也用券"), ("晴天", "周杰伦"), ("后来", "刘若英")]):
        tmp = os.path.join(enc_dir, f"_tmp{i}.flac")
        sine(tmp, 4, 44100, 16)
        art = cover(["gradient", "sun", "bokeh"][i], [["#1b2a3a", "#8fb3c9", "#e9d8b4"], ["#2a1d14", "#d9a066", "#3a2a1c"],
                                                       ["#2a1630", "#c98aa8", "#f2d8c9"]][i])
        make_ncm(os.path.join(enc_dir, f"{artist} - {title}.ncm"), open(tmp, "rb").read(), title, artist, title, art)
        os.remove(tmp)
    open(os.path.join(enc_dir, "缓存 - 未知.kgg"), "wb").write(os.urandom(4096))
except ImportError:
    print("cryptography 未安装,跳过加密样本")

print("demo library:", OUT)
