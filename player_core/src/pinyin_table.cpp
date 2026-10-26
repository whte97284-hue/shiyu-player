#include "../include/pinyin_table.hpp"
#include <cstdint>
#include <cwctype>

namespace {
#include "pinyin_data.inc"

// 将文本切分为发音单元:汉字 -> 拼音音节,假名 -> 罗马音音节,ASCII 字母数字 -> 原样小写
struct Unit {
    std::string text;
    bool phonetic; // 来自汉字/假名
};

const char* KanaRomaji(wchar_t ch) {
    uint32_t c = static_cast<uint32_t>(ch);
    if (c >= 0x30A1 && c <= 0x30F6) c -= 0x60; // 片假名 -> 平假名
    if (c >= 0x3041 && c <= 0x3096) return kKanaRomaji[c - 0x3041];
    return nullptr;
}

bool IsSmallY(wchar_t ch) {
    uint32_t c = static_cast<uint32_t>(ch);
    if (c >= 0x30A1 && c <= 0x30F6) c -= 0x60;
    return c == 0x3083 || c == 0x3085 || c == 0x3087; // ゃゅょ
}

bool IsSokuon(wchar_t ch) { return ch == 0x3063 || ch == 0x30C3; } // っ ッ

std::vector<Unit> Split(const std::wstring& text) {
    std::vector<Unit> units;
    bool pendingSokuon = false;
    for (size_t i = 0; i < text.size(); ++i) {
        wchar_t ch = text[i];
        if (const char* py = PinyinHelper::GetCharPinyin(ch)) {
            units.push_back({py, true});
            pendingSokuon = false;
            continue;
        }
        if (IsSokuon(ch)) { pendingSokuon = true; continue; }
        if (const char* ro = KanaRomaji(ch)) {
            std::string s = ro;
            if (s.empty()) continue;
            // 拗音:き+ゃ -> kya,し+ゃ -> sha,ち+ゅ -> chu,じ+ょ -> jo
            if (IsSmallY(ch) && !units.empty() && units.back().phonetic) {
                std::string& prev = units.back().text;
                if (prev.size() >= 2 && prev.back() == 'i') {
                    prev.pop_back();
                    bool palatal = (prev == "sh" || prev == "ch" || prev == "j");
                    prev += palatal ? s.substr(1) : s;
                    continue;
                }
            }
            if (pendingSokuon && !s.empty() && s[0] != 'a' && s[0] != 'i' && s[0] != 'u' && s[0] != 'e' && s[0] != 'o')
                s = s[0] + s;
            pendingSokuon = false;
            units.push_back({s, true});
            continue;
        }
        pendingSokuon = false;
        if (ch == 0x30FC) continue; // 长音符 ー
        if (ch < 128 && std::iswalnum(static_cast<wint_t>(ch))) {
            char c = static_cast<char>(std::towlower(static_cast<wint_t>(ch)));
            if (!units.empty() && !units.back().phonetic && !units.back().text.empty())
                units.back().text += c;
            else
                units.push_back({std::string(1, c), false});
            continue;
        }
        // 全角字母数字 -> 半角
        if (ch >= 0xFF10 && ch <= 0xFF5A && std::iswalnum(static_cast<wint_t>(ch - 0xFEE0))) {
            char c = static_cast<char>(std::towlower(static_cast<wint_t>(ch - 0xFEE0)));
            units.push_back({std::string(1, c), false});
            continue;
        }
        units.push_back({"", false}); // 分隔符
    }
    return units;
}

std::wstring Lower(const std::wstring& s) {
    std::wstring r = s;
    for (auto& ch : r) ch = static_cast<wchar_t>(std::towlower(static_cast<wint_t>(ch)));
    return r;
}

} // namespace

const char* PinyinHelper::GetCharPinyin(wchar_t ch) {
    uint32_t c = static_cast<uint32_t>(ch);
    if (c < kPinyinBase || c >= kPinyinBase + kPinyinCount) return nullptr;
    uint16_t idx = kPinyinIndex[c - kPinyinBase];
    return idx ? kSyllables[idx] : nullptr;
}

std::string PinyinHelper::GetFullPinyin(const std::wstring& text) {
    std::string result;
    for (const auto& u : Split(text)) result += u.text;
    return result;
}

std::string PinyinHelper::GetPinyinAcronym(const std::wstring& text) {
    std::string result;
    for (const auto& u : Split(text)) {
        if (u.text.empty()) continue;
        if (u.phonetic) result += u.text[0];
        else result += u.text; // 英文/数字保持原样 (G.E.M. -> gem)
    }
    return result;
}

std::string PinyinHelper::GetPinyinKey(const std::wstring& text) {
    std::string result;
    bool prevPhonetic = false;
    for (const auto& u : Split(text)) {
        if (u.text.empty()) {
            if (!result.empty() && result.back() != ' ') result += ' ';
            prevPhonetic = false;
            continue;
        }
        if (u.phonetic && prevPhonetic) result += '\'';
        else if (!result.empty() && result.back() != ' ' && result.back() != '\'') result += '\'';
        result += u.text;
        prevPhonetic = u.phonetic;
    }
    return result;
}

bool PinyinHelper::HasPhonetic(const std::wstring& text) {
    for (wchar_t ch : text)
        if (GetCharPinyin(ch) || KanaRomaji(ch)) return true;
    return false;
}

bool PinyinHelper::Matches(const std::wstring& target, const std::wstring& query) {
    if (query.empty()) return true;
    if (target.empty()) return false;

    // 1. 直接大小写无关子串匹配
    std::wstring lTarget = Lower(target), lQuery = Lower(query);
    if (lTarget.find(lQuery) != std::wstring::npos) return true;

    // 2. 汉字/假名查询:按拼音键做简繁/同音匹配 (至少两个音节,避免单字同音误命中)
    if (HasPhonetic(query)) {
        std::string qk = GetPinyinKey(query);
        if (qk.find('\'') != std::string::npos) {
            std::string tk = "'" + GetPinyinKey(target) + "'";
            for (auto& ch : tk) if (ch == ' ') ch = '\'';
            if (tk.find("'" + qk + "'") != std::string::npos) return true;
        }
        return false;
    }

    // 3. ASCII 查询:拼音首字母与全拼匹配 (如 "dzq" / "dengziqi" 匹配 "邓紫棋")
    std::string qAscii;
    for (wchar_t c : lQuery) {
        if (c < 128 && std::iswalnum(static_cast<wint_t>(c))) qAscii += static_cast<char>(c);
    }
    if (qAscii.empty()) return false;
    if (GetPinyinAcronym(target).find(qAscii) != std::string::npos) return true;
    if (GetFullPinyin(target).find(qAscii) != std::string::npos) return true;
    return false;
}
