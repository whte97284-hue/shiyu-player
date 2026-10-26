#pragma once
#include <string>
#include <vector>
#include <unordered_map>
#include <algorithm>

// ============================================================================
//  PinyinHelper —— 全量汉字拼音 + 日文假名罗马音检索引擎
//   · 覆盖 CJK 扩展 A + 基本区 26,703 个汉字 (简繁体通吃),由 tools/gen_pinyin_table.py 生成
//   · 平/片假名转罗马音 (含拗音、促音),「アダマス」可用 "adamasu" 搜到
//   · 拼音键 (音节以 ' 分隔) 支持简繁/同音互搜:「陈奕迅」可命中「陳奕迅」
// ============================================================================
class PinyinHelper {
public:
    // 获取中文字符串的完整拼音 (小写无音调，如 "邓紫棋" -> "dengziqi")
    static std::string GetFullPinyin(const std::wstring& text);

    // 获取拼音首字母缩写 (小写，如 "邓紫棋" -> "dzq")
    static std::string GetPinyinAcronym(const std::wstring& text);

    // 获取以 ' 分隔音节的拼音键 (如 "陳奕迅" -> "chen'yi'xun"),用于简繁/同音字互搜
    static std::string GetPinyinKey(const std::wstring& text);

    // 字符串中是否包含可转拼音/罗马音的汉字或假名
    static bool HasPhonetic(const std::wstring& text);

    // 判断搜索词 query 是否与 target 匹配 (支持中文、全拼、拼音首字母匹配)
    static bool Matches(const std::wstring& target, const std::wstring& query);

    // 获取单个字符的拼音 (无拼音返回 nullptr)
    static const char* GetCharPinyin(wchar_t ch);
};
