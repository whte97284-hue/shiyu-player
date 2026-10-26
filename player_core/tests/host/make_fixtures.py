#!/usr/bin/env python3
"""
生成解析内核的真实测试样本集与期望值清单 (manifest.json)。

- 音频由 ffmpeg 真实编码 (FLAC/MP3/Vorbis/Opus/ALAC/AAC/WAV/AIFF);
- 标签与封面由 mutagen 写入,另有若干手工构造的边界样本
  (ID3v2.2、整标签反同步、仅 ID3v1 GBK、ID3 前缀 FLAC、DSF/DFF/APE/NCM、RF64 等);
- 时长/采样率/声道/位深的期望值来自 ffprobe 或格式定义,作为独立的 ground truth。

用法: python3 make_fixtures.py <输出目录>
"""
import base64
import io
import json
import os
import struct
import subprocess
import sys

from PIL import Image
from mutagen.flac import FLAC, Picture
from mutagen.id3 import (APIC, COMM, ID3, TALB, TCON, TDRC, TIT2, TPE1, TPE2, TPOS, TRCK, TYER)
from mutagen.mp4 import MP4, MP4Cover
from mutagen.oggopus import OggOpus
from mutagen.oggvorbis import OggVorbis
from mutagen.oggflac import OggFLAC
from mutagen.aiff import AIFF
from mutagen.wave import WAVE

OUT = sys.argv[1] if len(sys.argv) > 1 else "fixtures"
os.makedirs(OUT, exist_ok=True)
manifest = []


def p(name):
    return os.path.join(OUT, name)


def ff(args):
    subprocess.run(["ffmpeg", "-v", "error", "-y", "-fflags", "+bitexact"] + args, check=True)


def sine(path, secs, rate, ch, codec_args, fmt_args=None):
    ff(["-f", "lavfi", "-i", f"sine=frequency=440:duration={secs}:sample_rate={rate}",
        "-ac", str(ch), "-map_metadata", "-1"] + codec_args + (fmt_args or []) + [path])


def probe(path):
    out = subprocess.run(["ffprobe", "-v", "error", "-select_streams", "a:0", "-show_entries",
                          "stream=sample_rate,channels,bits_per_raw_sample,bits_per_sample,duration,duration_ts,time_base:format=duration",
                          "-of", "json", path], capture_output=True, text=True, check=True).stdout
    j = json.loads(out)
    s = j["streams"][0]
    dur = float(s.get("duration") or j["format"]["duration"])
    bits = int(s.get("bits_per_raw_sample") or 0) or int(s.get("bits_per_sample") or 0)
    return {"sample_rate": int(s["sample_rate"]), "channels": int(s["channels"]),
            "duration_ms": int(round(dur * 1000)), "bits": bits}


def img(w, h, fmt, color=(200, 30, 60), noise=False):
    im = Image.new("RGB", (w, h), color)
    if noise:
        im = Image.frombytes("RGB", (w, h), os.urandom(w * h * 3))
    b = io.BytesIO()
    im.save(b, format=fmt, quality=95)
    return b.getvalue()


FRONT_PNG = img(500, 500, "PNG")
BACK_JPG = img(300, 200, "JPEG", (20, 120, 220))
BIG_JPG = img(1400, 1400, "JPEG", noise=True)  # ~2-4 MB,模拟高清母带大封面


def add(name, expect, dur_tol=5, true_ms=None, **extra):
    # true_ms: 带 LAME/Lavc 无缝信息的 MP3,内核会扣除编码器延迟与补齐,得到源音频的真实时长;
    # ffprobe 的 stream duration 则是未裁剪的帧时长 (多出约 1~2 帧),因此以真实时长为准
    if true_ms is not None:
        expect = dict(expect, duration_ms=true_ms)
        dur_tol = 2
    e = {"file": name, "expect": expect, "dur_tol": dur_tol}
    e.update(extra)
    manifest.append(e)


TAGS_CN = dict(title="后来", artist="刘若英", album="我等你", album_artist="刘若英", genre="流行",
               year="1999", track="3", disc="1")

# ---------------------------------------------------------------- FLAC
def flac_tags(f, t):
    f["TITLE"] = t["title"]
    f["ARTIST"] = t["artist"]
    f["ALBUM"] = t["album"]
    f["ALBUMARTIST"] = t["album_artist"]
    f["GENRE"] = t["genre"]
    f["DATE"] = t["year"]
    f["TRACKNUMBER"] = t["track"] + "/12"
    f["DISCNUMBER"] = t["disc"]


def flac_pic(data, ptype, mime, w, h):
    pic = Picture()
    pic.type = ptype
    pic.mime = mime
    pic.width, pic.height, pic.depth = w, h, 24
    pic.data = data
    return pic


name = "flac_16_44_tags_two_covers.flac"
sine(p(name), 3, 44100, 2, ["-c:a", "flac"])
f = FLAC(p(name))
flac_tags(f, TAGS_CN)
f.add_picture(flac_pic(BACK_JPG, 4, "image/jpeg", 300, 200))   # 背面封面在前
f.add_picture(flac_pic(FRONT_PNG, 3, "image/png", 500, 500))   # 正面封面在后,应被优选
f.save()
add(name, dict(probe(p(name)), format="FLAC", codec="FLAC", lossless=True, **TAGS_CN,
               has_cover=True, cover_type=3, cover_mime="image/png", cover_len=len(FRONT_PNG),
               cover_w=500, cover_h=500), dur_tol=1)

name = "flac_24_96_bigcover_first.flac"
sine(p(name), 4, 96000, 2, ["-c:a", "flac", "-sample_fmt", "s32", "-bits_per_raw_sample", "24"])
f = FLAC(p(name))
f.add_picture(flac_pic(BIG_JPG, 3, "image/jpeg", 1400, 1400))
f.save()
# mutagen 写入顺序为 STREAMINFO -> 其他 -> VORBIS_COMMENT;重写把 PICTURE 挪到 VORBIS_COMMENT 之前
f = FLAC(p(name))
flac_tags(f, dict(TAGS_CN, title="Last Kiss 24bit"))
f.metadata_blocks.sort(key=lambda b: {0: 0, 6: 1}.get(b.code, 2))
f.save()
add(name, dict(probe(p(name)), format="FLAC", codec="FLAC", lossless=True, **dict(TAGS_CN, title="Last Kiss 24bit"),
               has_cover=True, cover_type=3, cover_mime="image/jpeg", cover_len=len(BIG_JPG)),
    dur_tol=1, lazy_max_io_bytes=256 * 1024)

name = "flac_24_192_mono.flac"
sine(p(name), 2, 192000, 1, ["-c:a", "flac", "-sample_fmt", "s32", "-bits_per_raw_sample", "24"])
add(name, dict(probe(p(name)), format="FLAC", lossless=True, has_cover=False), dur_tol=1)

# FLAC 被某些软件错误地加上 ID3v2 头
name = "flac_with_id3_prefix.flac"
sine(p(name), 2, 48000, 2, ["-c:a", "flac"])
f = FLAC(p(name))
f["TITLE"] = "FLAC 真标签"
f.save()
raw = open(p(name), "rb").read()
id3 = ID3()
id3.add(TIT2(encoding=3, text="ID3 前缀标题"))
id3.add(TPE1(encoding=3, text="前缀歌手"))
bio = io.BytesIO()
id3.save(bio, v2_version=3)
open(p(name), "wb").write(bio.getvalue() + raw)
add(name, dict(probe(p(name)), format="FLAC", title="FLAC 真标签", artist="前缀歌手"), dur_tol=1)

# ---------------------------------------------------------------- MP3
def id3_tags(t, v, enc=3):
    tag = ID3()
    tag.add(TIT2(encoding=enc, text=t["title"]))
    tag.add(TPE1(encoding=enc, text=t["artist"]))
    tag.add(TALB(encoding=enc, text=t["album"]))
    tag.add(TPE2(encoding=enc, text=t["album_artist"]))
    tag.add(TCON(encoding=enc, text=t["genre"]))
    if v == 4:
        tag.add(TDRC(encoding=enc, text=t["year"]))
    else:
        tag.add(TYER(encoding=enc, text=t["year"]))
    tag.add(TRCK(encoding=enc, text=t["track"] + "/10"))
    tag.add(TPOS(encoding=enc, text=t["disc"] + "/2"))
    tag.add(COMM(encoding=enc, lang="chi", desc="", text="测试注释"))
    return tag


name = "mp3_cbr320_id3v23_utf16_apic.mp3"
sine(p(name), 5, 44100, 2, ["-c:a", "libmp3lame", "-b:a", "320k"])
tag = id3_tags(TAGS_CN, 3, enc=1)
tag.add(APIC(encoding=1, mime="image/jpeg", type=3, desc="封面", data=BIG_JPG))
tag.save(p(name), v2_version=3)
add(name, dict(probe(p(name)), format="MP3", codec="MP3", **TAGS_CN, comment="测试注释",
               has_cover=True, cover_type=3, cover_mime="image/jpeg", cover_len=len(BIG_JPG)),
    true_ms=5000, lazy_max_io_bytes=160 * 1024)

name = "mp3_vbr_id3v24_utf8.mp3"
sine(p(name), 7, 48000, 2, ["-c:a", "libmp3lame", "-q:a", "2"])
tag = id3_tags(dict(TAGS_CN, title="稻香 VBR"), 4, enc=3)
tag.add(APIC(encoding=3, mime="image/png", type=3, desc="", data=FRONT_PNG))
tag.save(p(name), v2_version=4)
add(name, dict(probe(p(name)), format="MP3", **dict(TAGS_CN, title="稻香 VBR"), has_cover=True,
               cover_mime="image/png", cover_len=len(FRONT_PNG), cover_w=500, cover_h=500), true_ms=7000)

name = "mp3_mpeg2_22k_mono.mp3"
sine(p(name), 3, 22050, 1, ["-c:a", "libmp3lame", "-b:a", "64k"])
add(name, dict(probe(p(name)), format="MP3", has_cover=False), true_ms=3000)

# 无任何标签的裸 MP3 (无 Xing 头的纯 CBR)
name = "mp3_raw_cbr_noxing.mp3"
sine(p(name), 4, 44100, 2, ["-c:a", "libmp3lame", "-b:a", "128k", "-write_xing", "0"])
add(name, dict(probe(p(name)), format="MP3", has_cover=False), dur_tol=60)

# 仅 ID3v1,文本为 GBK 编码 (国内老 MP3 的典型情况)
name = "mp3_id3v1_gbk.mp3"
sine(p(name), 3, 44100, 2, ["-c:a", "libmp3lame", "-b:a", "128k"])
def v1field(s, n):
    b = s.encode("gbk")[:n]
    return b + b"\0" * (n - len(b))
v1 = b"TAG" + v1field("十年", 30) + v1field("陈奕迅", 30) + v1field("黑白灰", 30) + b"2003" + \
     v1field("", 28) + b"\0" + bytes([7]) + bytes([13])
open(p(name), "ab").write(v1)
add(name, dict(probe(p(name)), format="MP3", title="十年", artist="陈奕迅", album="黑白灰", year="2003",
               track="7", genre="Pop"), true_ms=3000)


# 手工构造 ID3v2.2 (3 字符帧 ID) + PIC
def v22_frame(fid, payload):
    return fid + struct.pack(">I", len(payload))[1:] + payload


def syncsafe(n):
    return bytes([(n >> 21) & 0x7F, (n >> 14) & 0x7F, (n >> 7) & 0x7F, n & 0x7F])


name = "mp3_id3v22.mp3"
sine(p(name), 3, 44100, 2, ["-c:a", "libmp3lame", "-b:a", "192k"])
frames = v22_frame(b"TT2", b"\x01" + "老标签 v2.2".encode("utf-16")) + \
         v22_frame(b"TP1", b"\x00Old Artist") + \
         v22_frame(b"TCO", b"\x00(13)") + \
         v22_frame(b"PIC", b"\x00PNG\x03\x00" + FRONT_PNG)
body = frames + b"\0" * 64
open(p(name), "r+b").write  # noqa
audio = open(p(name), "rb").read()
open(p(name), "wb").write(b"ID3\x02\x00\x00" + syncsafe(len(body)) + body + audio)
add(name, dict(probe(p(name)), format="MP3", title="老标签 v2.2", artist="Old Artist", genre="Pop",
               has_cover=True, cover_mime="image/png", cover_len=len(FRONT_PNG)), true_ms=3000)


# 手工构造整标签反同步的 ID3v2.3 (封面中含大量 FF 00 / FF Ex)
def v23_frame(fid, payload):
    return fid + struct.pack(">I", len(payload)) + b"\0\0" + payload


def unsync(b):
    out = bytearray()
    for i, x in enumerate(b):
        out.append(x)
        if x == 0xFF and (i + 1 == len(b) or b[i + 1] >= 0xE0 or b[i + 1] == 0x00):
            out.append(0)
    return bytes(out)


name = "mp3_id3v23_unsync.mp3"
sine(p(name), 3, 44100, 2, ["-c:a", "libmp3lame", "-b:a", "128k"])
frames = v23_frame(b"TIT2", b"\x03" + "反同步标题".encode()) + \
         v23_frame(b"TPE1", b"\x03" + "反同步歌手".encode()) + \
         v23_frame(b"APIC", b"\x00image/jpeg\x00\x03\x00" + BACK_JPG)
body = unsync(frames) + b"\0" * 32
audio = open(p(name), "rb").read()
open(p(name), "wb").write(b"ID3\x03\x00\x80" + syncsafe(len(body)) + body + audio)
add(name, dict(probe(p(name)), format="MP3", title="反同步标题", artist="反同步歌手", has_cover=True,
               cover_mime="image/jpeg", cover_len=len(BACK_JPG), cover_w=300, cover_h=200), true_ms=3000)

# ---------------------------------------------------------------- Ogg
name = "ogg_vorbis_tags_pic.ogg"
sine(p(name), 4, 44100, 2, ["-c:a", "libvorbis", "-q:a", "4"])
o = OggVorbis(p(name))
for k, v in dict(title="夜曲", artist="周杰伦", album="十一月的萧邦", albumartist="周杰伦", genre="流行",
                 date="2005", tracknumber="1", discnumber="1").items():
    o[k] = v
o["ARTIST"] = ["周杰伦", "方文山"]
pic = flac_pic(FRONT_PNG, 3, "image/png", 500, 500)
o["metadata_block_picture"] = [base64.b64encode(pic.write()).decode()]
o.save()
add(name, dict(probe(p(name)), format="OGG", codec="Vorbis", title="夜曲", artist="周杰伦/方文山",
               album="十一月的萧邦", album_artist="周杰伦", year="2005", has_cover=True,
               cover_mime="image/png", cover_len=len(FRONT_PNG)), dur_tol=5)

name = "opus_tags.opus"
sine(p(name), 3, 48000, 2, ["-c:a", "libopus", "-b:a", "96k"])
o = OggOpus(p(name))
o["title"] = "Opus 测试"
o["artist"] = "YOASOBI"
o.save()
add(name, dict(probe(p(name)), format="OPUS", codec="Opus", title="Opus 测试", artist="YOASOBI"), dur_tol=25)

name = "ogg_flac.oga"
sine(p(name), 2, 44100, 2, ["-c:a", "flac", "-f", "ogg"])
o = OggFLAC(p(name))
o["title"] = "Ogg FLAC 标题"
o.save()
add(name, dict(probe(p(name)), format="OGG", codec="FLAC", lossless=True, title="Ogg FLAC 标题"), dur_tol=5)

# ---------------------------------------------------------------- WAV / AIFF
name = "wav_16_44_info_gbk_id3_tail.wav"
sine(p(name), 3, 44100, 2, ["-c:a", "pcm_s16le"])
w = WAVE(p(name))
w.add_tags()
w.tags.add(TIT2(encoding=3, text="River Flows in You"))
w.tags.add(TPE1(encoding=3, text="Yiruma"))
w.tags.add(APIC(encoding=3, mime="image/jpeg", type=3, desc="", data=BACK_JPG))
w.save()
# 追加 GBK 编码的 LIST/INFO 块 (含专辑名)
info = b"INFO"
for fid, text in ((b"IPRD", "初恋".encode("gbk") + b"\0"), (b"IGNR", b"New Age\0")):
    info += fid + struct.pack("<I", len(text)) + text + (b"\0" if len(text) % 2 else b"")
lst = b"LIST" + struct.pack("<I", len(info)) + info
raw = bytearray(open(p(name), "rb").read() + lst)
struct.pack_into("<I", raw, 4, len(raw) - 8)
open(p(name), "wb").write(bytes(raw))
add(name, dict(probe(p(name)), format="WAV", codec="PCM", lossless=True, title="River Flows in You",
               artist="Yiruma", album="初恋", genre="New Age", has_cover=True, cover_len=len(BACK_JPG)), dur_tol=1)

name = "wav_24_96_extensible.wav"
sine(p(name), 2, 96000, 2, ["-c:a", "pcm_s24le", "-rf64", "never"], ["-write_bext", "0"])
add(name, dict(probe(p(name)), format="WAV", lossless=True), dur_tol=1)

name = "wav_f32_48.wav"
sine(p(name), 2, 48000, 2, ["-c:a", "pcm_f32le"])
add(name, dict(probe(p(name)), format="WAV", codec="PCM Float", lossless=True), dur_tol=1)

name = "wav_rf64.wav"
sine(p(name), 2, 48000, 2, ["-c:a", "pcm_s24le", "-rf64", "always"])
add(name, dict(probe(p(name)), format="WAV", lossless=True), dur_tol=1)

name = "aiff_16_48_id3.aiff"
sine(p(name), 3, 48000, 2, ["-c:a", "pcm_s16be"])
a = AIFF(p(name))
a.add_tags()
a.tags.add(TIT2(encoding=3, text="AIFF 标题"))
a.tags.add(TPE1(encoding=3, text="AIFF 歌手"))
a.save()
add(name, dict(probe(p(name)), format="AIFF", lossless=True, title="AIFF 标题", artist="AIFF 歌手"), dur_tol=1)

# ---------------------------------------------------------------- MP4
def mp4_tags(m, t, cover, fmt):
    m["\xa9nam"] = t["title"]
    m["\xa9ART"] = t["artist"]
    m["\xa9alb"] = t["album"]
    m["aART"] = t["album_artist"]
    m["\xa9gen"] = t["genre"]
    m["\xa9day"] = t["year"]
    m["trkn"] = [(int(t["track"]), 12)]
    m["disk"] = [(int(t["disc"]), 2)]
    m["covr"] = [MP4Cover(cover, imageformat=fmt)]


name = "m4a_alac_24_96_moov_end.m4a"
sine(p(name), 3, 96000, 2, ["-c:a", "alac", "-sample_fmt", "s32p"])
m = MP4(p(name))
mp4_tags(m, TAGS_CN, BIG_JPG, MP4Cover.FORMAT_JPEG)
m.save()
add(name, dict(probe(p(name)), format="M4A", codec="ALAC", lossless=True, **TAGS_CN, has_cover=True,
               cover_mime="image/jpeg", cover_len=len(BIG_JPG)), dur_tol=2, lazy_max_io_bytes=256 * 1024)

name = "m4a_aac_faststart.m4a"
sine(p(name), 5, 44100, 2, ["-c:a", "aac", "-b:a", "192k", "-movflags", "+faststart"])
m = MP4(p(name))
mp4_tags(m, dict(TAGS_CN, title="AAC 标题"), FRONT_PNG, MP4Cover.FORMAT_PNG)
m.save()
add(name, dict(probe(p(name)), format="M4A", codec="AAC", **dict(TAGS_CN, title="AAC 标题"), has_cover=True,
               cover_mime="image/png", cover_len=len(FRONT_PNG), cover_w=500, cover_h=500, bits=16), dur_tol=30)

# ---------------------------------------------------------------- DSD (手工构造)
def id3_bytes(title, artist, cover=None):
    tag = ID3()
    tag.add(TIT2(encoding=3, text=title))
    tag.add(TPE1(encoding=3, text=artist))
    if cover:
        tag.add(APIC(encoding=3, mime="image/jpeg", type=3, desc="", data=cover))
    b = io.BytesIO()
    tag.save(b, v2_version=3)
    return b.getvalue()


name = "dsd64_tags.dsf"
rate, ch, secs = 2822400, 2, 2
samples = rate * secs
block = 4096
blocks_per_ch = (samples // 8 + block - 1) // block
data = bytes(blocks_per_ch * block * ch)
tag = id3_bytes("Lemon (DSD)", "米津玄師", BACK_JPG)
total = 28 + 52 + 12 + len(data) + len(tag)
dsd = b"DSD " + struct.pack("<QQQ", 28, total, 28 + 52 + 12 + len(data))
fmt = b"fmt " + struct.pack("<QIIIIIIQII", 52, 1, 0, 2, ch, rate, 1, samples, block, 0)
open(p(name), "wb").write(dsd + fmt + b"data" + struct.pack("<Q", 12 + len(data)) + data + tag)
add(name, dict(format="DSF", codec="DSD64", lossless=True, sample_rate=rate, channels=ch, bits=1,
               duration_ms=secs * 1000, title="Lemon (DSD)", artist="米津玄師", has_cover=True,
               cover_len=len(BACK_JPG)), dur_tol=1)


def dff_chunk(cid, payload):
    pad = b"\0" if len(payload) % 2 else b""
    return cid + struct.pack(">Q", len(payload)) + payload + pad


name = "dsd128_diin.dff"
rate, ch, secs = 5644800, 2, 1
snd = b"SND " + dff_chunk(b"FS  ", struct.pack(">I", rate)) + \
      dff_chunk(b"CHNL", struct.pack(">H", ch) + b"SLFTSRGT") + \
      dff_chunk(b"CMPR", b"DSD " + bytes([14]) + b"not compressed\0")
dsd_data = bytes(rate * secs * ch // 8)
diar = "上原れな".encode()
diti = "Shiny Days".encode()
diin = dff_chunk(b"DIAR", struct.pack(">I", len(diar)) + diar) + dff_chunk(b"DITI", struct.pack(">I", len(diti)) + diti)
body = b"DSD " + dff_chunk(b"FVER", struct.pack(">I", 0x01050000)) + dff_chunk(b"PROP", snd) + \
       dff_chunk(b"DSD ", dsd_data) + dff_chunk(b"DIIN", diin) + dff_chunk(b"ID3 ", id3_bytes("ID3 标题", "ID3 歌手"))
open(p(name), "wb").write(b"FRM8" + struct.pack(">Q", len(body)) + body)
add(name, dict(format="DFF", codec="DSD128", lossless=True, sample_rate=rate, channels=ch, bits=1,
               duration_ms=secs * 1000, title="Shiny Days", artist="上原れな"), dur_tol=1)

# ---------------------------------------------------------------- APE (手工构造头 + APEv2 标签)
name = "ape_v3990_apev2.ape"
rate, ch, bps = 44100, 2, 16
bpf, total_frames, final_blocks = 73728 * 4, 3, 1000
desc = b"MAC " + struct.pack("<HH", 3990, 0) + struct.pack("<IIIIIII", 52, 24, 0, 0, 1000, 0, 0) + bytes(16)
hdr = struct.pack("<HHIIIHHI", 2000, 0, bpf, final_blocks, total_frames, bps, ch, rate)
payload = bytes(1000)
def ape_item(key, val, flags=0):
    return struct.pack("<II", len(val), flags) + key + b"\0" + val
items = ape_item(b"Title", "红豆".encode()) + ape_item(b"Artist", "王菲".encode()) + \
        ape_item(b"Album", "唱游".encode()) + ape_item(b"Track", b"5/10") + \
        ape_item(b"Cover Art (Front)", b"cover.jpg\0" + BACK_JPG, flags=2)
footer = b"APETAGEX" + struct.pack("<IIII", 2000, len(items) + 32, 5, 0) + bytes(8)
open(p(name), "wb").write(desc + hdr + payload + items + footer)
dur_ms = ((total_frames - 1) * bpf + final_blocks) * 1000 // rate
add(name, dict(format="APE", codec="APE", lossless=True, sample_rate=rate, channels=ch, bits=bps,
               duration_ms=dur_ms, title="红豆", artist="王菲", album="唱游", track="5", has_cover=True,
               cover_len=len(BACK_JPG)), dur_tol=1)

# ---------------------------------------------------------------- NCM (手工构造加密元数据)
from cryptography.hazmat.primitives.ciphers import Cipher, algorithms, modes
name = "netease.ncm"
meta_json = json.dumps({"musicId": 1, "musicName": "起风了", "artist": [["买辣椒也用券", 1], ["林俊杰", 2]],
                        "album": "起风了", "bitrate": 999000, "duration": 325000, "format": "flac"},
                       ensure_ascii=False).encode()
plain = b"music:" + meta_json
padn = 16 - len(plain) % 16
plain += bytes([padn]) * padn
enc = Cipher(algorithms.AES(bytes.fromhex("2331346C6A6B5F215C5D2630553C2728")), modes.ECB()).encryptor()
ct = enc.update(plain) + enc.finalize()
meta_blob = bytes(b ^ 0x63 for b in (b"163 key(Don't modify):" + base64.b64encode(ct)))
# 真实可解密的 NCM:密钥块 = AES(core key, "neteasecloudmusic" + RC4 密钥) ^ 0x64,音频为 RC4 变体密钥流加密的 FLAC
rc4_key = b"E7fT49x7dof9OKCgg9cdvhEuezy3iZCL1nFvBFd1T4uSktAJKmwZXsijPbijliionVUXXg9plTbXEclAE9Lb"
kp = b"neteasecloudmusic" + rc4_key
kp += bytes([16 - len(kp) % 16]) * (16 - len(kp) % 16)
kenc = Cipher(algorithms.AES(bytes.fromhex("687A4852416D736F356B496E62617857")), modes.ECB()).encryptor()
key_blob = bytes(b ^ 0x64 for b in kenc.update(kp) + kenc.finalize())
box = list(range(256))
last, ko = 0, 0
for i in range(256):
    sw = box[i]
    c = (sw + last + rc4_key[ko]) & 0xFF
    ko = (ko + 1) % len(rc4_key)
    box[i], box[c] = box[c], sw
    last = c
stream = bytes(box[(box[(i + 1) & 0xFF] + box[(box[(i + 1) & 0xFF] + ((i + 1) & 0xFF)) & 0xFF]) & 0xFF] for i in range(256))
NCM_SRC = os.path.join(OUT, "ncm_source")
os.makedirs(NCM_SRC, exist_ok=True)
ncm_plain_path = os.path.join(NCM_SRC, "plain.flac")
sine(ncm_plain_path, 2, 44100, 2, ["-c:a", "flac"])
ncm_plain = open(ncm_plain_path, "rb").read()
ncm_audio = bytes(b ^ stream[i & 0xFF] for i, b in enumerate(ncm_plain))
ncm = b"CTENFDAM" + b"\x01\x70" + struct.pack("<I", len(key_blob)) + key_blob + \
      struct.pack("<I", len(meta_blob)) + meta_blob + b"\0\0\0\0" + bytes(5) + \
      struct.pack("<I", len(BACK_JPG)) + BACK_JPG + ncm_audio
open(p(name), "wb").write(ncm)
add(name, dict(format="NCM", codec="FLAC", lossless=True, title="起风了", artist="买辣椒也用券/林俊杰",
               album="起风了", duration_ms=325000, has_cover=True, cover_len=len(BACK_JPG)), dur_tol=0)


# ---------------------------------------------------------------- 入库流水线专用素材 (不进 manifest)
LIB = os.path.join(OUT, "library_extras")
os.makedirs(os.path.join(LIB, "cue_album"), exist_ok=True)
os.makedirs(os.path.join(LIB, "eason", "CD1"), exist_ok=True)
os.makedirs(os.path.join(LIB, "jp"), exist_ok=True)

# CUE 整轨:30 秒 FLAC + GBK 编码 CUE (FILE 写的是 .wav,实际文件为 .flac)
img_path = os.path.join(LIB, "cue_album", "CDImage.flac")
sine(img_path, 30, 44100, 2, ["-c:a", "flac"])
cue = """REM GENRE Pop
REM DATE 2003
PERFORMER "陈奕迅"
TITLE "黑白灰"
FILE "CDImage.wav" WAVE
  TRACK 01 AUDIO
    TITLE "十年"
    PERFORMER "陈奕迅"
    INDEX 01 00:00:00
  TRACK 02 AUDIO
    TITLE "明年今日"
    INDEX 00 00:09:50
    INDEX 01 00:10:00
  TRACK 03 AUDIO
    TITLE "黑白灰"
    INDEX 01 00:20:37
"""
open(os.path.join(LIB, "cue_album", "CDImage.cue"), "wb").write(cue.replace("\n", "\r\n").encode("gbk"))
open(os.path.join(LIB, "cue_album", "folder.jpg"), "wb").write(BACK_JPG)

# 同一首歌的三个版本:24/96 FLAC、16/44 FLAC、MP3,用于去重择优;繁体歌手名用于简繁互搜
for fname, args, rate in (("黑夜不再来_hires.flac", ["-c:a", "flac", "-sample_fmt", "s32", "-bits_per_raw_sample", "24"], 96000),
                          ("黑夜不再来_cd.flac", ["-c:a", "flac"], 44100),
                          ("黑夜不再来.mp3", ["-c:a", "libmp3lame", "-b:a", "320k"], 44100)):
    fp = os.path.join(LIB, "eason", "CD1", fname)
    sine(fp, 2, rate, 2, args)
    if fp.endswith(".flac"):
        f = FLAC(fp)
        f["TITLE"], f["ARTIST"], f["ALBUM"] = "黑夜不再来", "陳奕迅", "首选陈奕迅"
        f.save()
    else:
        t = ID3()
        t.add(TIT2(encoding=3, text="黑夜不再来"))
        t.add(TPE1(encoding=3, text="陳奕迅"))
        t.add(TALB(encoding=3, text="首选陈奕迅"))
        t.save(fp, v2_version=3)
open(os.path.join(LIB, "eason", "cover.jpg"), "wb").write(FRONT_PNG)  # 上级目录封面,CD1 应继承

fp = os.path.join(LIB, "jp", "アダマス.flac")
sine(fp, 2, 48000, 2, ["-c:a", "flac"])
f = FLAC(fp)
f["TITLE"], f["ARTIST"] = "アダマス", "LiSA"
f.save()
open(os.path.join(LIB, "jp", "readme.txt"), "w").write("not audio")
open(os.path.join(LIB, "jp", "broken.flac"), "wb").write(b"fLaC" + os.urandom(64))

# 真实库中发现的边界情况
os.makedirs(os.path.join(LIB, "__MACOSX", "jp"), exist_ok=True)
open(os.path.join(LIB, "__MACOSX", "jp", "._アダマス.flac"), "wb").write(b"\x00\x05\x16\x07" + bytes(200))
open(os.path.join(LIB, "jp", "._hidden.flac"), "wb").write(b"\x00\x05\x16\x07" + bytes(200))
os.makedirs(os.path.join(LIB, "video"), exist_ok=True)
ff(["-f", "lavfi", "-i", "testsrc=size=160x120:rate=10:duration=2", "-f", "lavfi", "-i", "sine=duration=2",
    "-c:v", "mpeg4", "-c:a", "aac", "-shortest", os.path.join(LIB, "video", "ep10 [1080P].mp4")])
open(os.path.join(LIB, "jp", "qq_mflac.flac"), "wb").write(os.urandom(50000) + b"\x00\x00\x01\x2cQTag")
open(os.path.join(LIB, "jp", "random_encrypted.flac"), "wb").write(os.urandom(50000))
os.makedirs(os.path.join(LIB, "YANKEE (Deluxe)", "CD1"), exist_ok=True)
sine(os.path.join(LIB, "YANKEE (Deluxe)", "CD1", "02. Lemon.wav"), 2, 44100, 2, ["-c:a", "pcm_s16le"])

# ReplayGain + 内嵌歌词 (不进 manifest,也不进入库测试曲库)
TAGX = os.path.join(OUT, "tag_extras")
os.makedirs(TAGX, exist_ok=True)
fp = os.path.join(TAGX, "rg.flac")
sine(fp, 1, 44100, 2, ["-c:a", "flac"])
f = FLAC(fp)
f["TITLE"] = "RG"
f["REPLAYGAIN_TRACK_GAIN"], f["REPLAYGAIN_TRACK_PEAK"] = "-6.50 dB", "0.950000"
f["REPLAYGAIN_ALBUM_GAIN"], f["REPLAYGAIN_ALBUM_PEAK"] = "-5.00 dB", "0.990000"
f["LYRICS"] = "[00:01.00]第一句\n[00:02.00]第二句"
f.save()
fp = os.path.join(TAGX, "rg.mp3")
sine(fp, 1, 44100, 2, ["-c:a", "libmp3lame", "-b:a", "128k"])
from mutagen.id3 import TXXX, USLT
t = ID3()
t.add(TIT2(encoding=3, text="RG mp3"))
t.add(TXXX(encoding=3, desc="REPLAYGAIN_TRACK_GAIN", text="+2.10 dB"))
t.add(USLT(encoding=1, lang="chi", desc="", text="[00:00.50]你好\n[00:01.00]世界"))
t.save(fp, v2_version=3)

json.dump(manifest, open(p("manifest.json"), "w", encoding="utf-8"), ensure_ascii=False, indent=1)
print(f"generated {len(manifest)} fixtures in {OUT}")
