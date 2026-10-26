#!/usr/bin/env python3
"""
生成 src/pinyin_data.inc:覆盖 CJK 统一汉字扩展 A + 基本区 (U+3400..U+9FFF) 共 27,648 个码位的
无声调拼音索引表 (含繁体字),以及平/片假名到罗马音的映射。
依赖: pip install pypinyin
"""
import sys
from pypinyin import pinyin, Style

LO, HI = 0x3400, 0x9FFF
syllables = {}
idx = []
for cp in range(LO, HI + 1):
    ch = chr(cp)
    py = pinyin(ch, style=Style.NORMAL, heteronym=False, errors=lambda x: [""])[0][0]
    py = py.replace("ü", "v").lower()
    if not py or not py.isascii() or not py.isalpha():
        idx.append(0)
        continue
    if py not in syllables:
        syllables[py] = len(syllables) + 1
    idx.append(syllables[py])

names = sorted(syllables, key=lambda s: syllables[s])
out = []
out.append("// 由 tools/gen_pinyin_table.py 自动生成,请勿手工编辑")
out.append(f"// 覆盖 U+{LO:04X}..U+{HI:04X},有拼音的汉字 {sum(1 for i in idx if i)} 个,音节 {len(names)} 个")
out.append(f"static const uint32_t kPinyinBase = 0x{LO:04X};")
out.append(f"static const uint32_t kPinyinCount = {len(idx)};")
out.append("static const char* const kSyllables[] = {\"\",")
line = "   "
for n in names:
    item = f' "{n}",'
    if len(line) + len(item) > 110:
        out.append(line)
        line = "   "
    line += item
out.append(line)
out.append("};")
out.append(f"static const uint16_t kPinyinIndex[{len(idx)}] = {{")
for i in range(0, len(idx), 24):
    out.append("    " + ",".join(str(x) for x in idx[i:i + 24]) + ",")
out.append("};")

# 假名 -> 罗马音 (平假名 U+3041..U+3096, 片假名 = 平假名 + 0x60)
kana = {
 "あ":"a","い":"i","う":"u","え":"e","お":"o","か":"ka","き":"ki","く":"ku","け":"ke","こ":"ko",
 "さ":"sa","し":"shi","す":"su","せ":"se","そ":"so","た":"ta","ち":"chi","つ":"tsu","て":"te","と":"to",
 "な":"na","に":"ni","ぬ":"nu","ね":"ne","の":"no","は":"ha","ひ":"hi","ふ":"fu","へ":"he","ほ":"ho",
 "ま":"ma","み":"mi","む":"mu","め":"me","も":"mo","や":"ya","ゆ":"yu","よ":"yo",
 "ら":"ra","り":"ri","る":"ru","れ":"re","ろ":"ro","わ":"wa","ゐ":"i","ゑ":"e","を":"o","ん":"n",
 "が":"ga","ぎ":"gi","ぐ":"gu","げ":"ge","ご":"go","ざ":"za","じ":"ji","ず":"zu","ぜ":"ze","ぞ":"zo",
 "だ":"da","ぢ":"ji","づ":"zu","で":"de","ど":"do","ば":"ba","び":"bi","ぶ":"bu","べ":"be","ぼ":"bo",
 "ぱ":"pa","ぴ":"pi","ぷ":"pu","ぺ":"pe","ぽ":"po","ぁ":"a","ぃ":"i","ぅ":"u","ぇ":"e","ぉ":"o",
 "ゃ":"ya","ゅ":"yu","ょ":"yo","っ":"","ゎ":"wa","ゔ":"vu","ゕ":"ka","ゖ":"ke",
}
out.append("static const char* const kKanaRomaji[0x56] = {")
row = []
for cp in range(0x3041, 0x3097):
    row.append('"' + kana.get(chr(cp), "") + '"')
for i in range(0, len(row), 16):
    out.append("    " + ", ".join(row[i:i + 16]) + ",")
out.append("};")

path = sys.argv[1] if len(sys.argv) > 1 else "src/pinyin_data.inc"
open(path, "w", encoding="utf-8").write("\n".join(out) + "\n")
print(f"wrote {path}: {sum(1 for i in idx if i)} hanzi, {len(names)} syllables")
