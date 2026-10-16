// =============================================================================
//  字幕キューストア (src/Core/SubtitleSource.cpp)
// -----------------------------------------------------------------------------
//  SRT / ASS / SSA (サイドカー) を直接パースして全件ロードする。
//  埋め込み字幕は VideoDecoder のデコードスレッドが add() で流し込む。
// =============================================================================
#include "Core/SubtitleSource.hpp"

#include "Core/Log.hpp"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <cwctype>
#include <fstream>
#include <iterator>
#include <mutex>
#include <sstream>
#include <vector>

namespace nanoalpha {
namespace {

// ---------------------------------------------------------------------------
//  ユーティリティ
// ---------------------------------------------------------------------------
std::string utf8FromWide(const std::wstring& text) {
    if (text.empty()) return {};
    const int need = WideCharToMultiByte(CP_UTF8, 0, text.c_str(),
                                         static_cast<int>(text.size()), nullptr, 0,
                                         nullptr, nullptr);
    if (need <= 0) return {};
    std::string out(static_cast<std::size_t>(need), '\0');
    WideCharToMultiByte(CP_UTF8, 0, text.c_str(), static_cast<int>(text.size()),
                        out.data(), need, nullptr, nullptr);
    return out;
}

std::string toLowerExt(const std::wstring& path) {
    const std::size_t dot = path.find_last_of(L'.');
    const std::size_t sep = path.find_last_of(L"/\\");
    if (dot == std::wstring::npos || (sep != std::wstring::npos && dot < sep)) return {};
    std::wstring ext = path.substr(dot + 1);
    for (auto& ch : ext) ch = static_cast<wchar_t>(towlower(ch));
    return utf8FromWide(ext);
}

bool allDigits(const std::string& s) {
    if (s.empty()) return false;
    for (char c : s) {
        if (!std::isdigit(static_cast<unsigned char>(c))) return false;
    }
    return true;
}

std::string trimCopy(const std::string& s) {
    std::size_t b = 0;
    std::size_t e = s.size();
    while (b < e && std::isspace(static_cast<unsigned char>(s[b]))) ++b;
    while (e > b && std::isspace(static_cast<unsigned char>(s[e - 1]))) --e;
    return s.substr(b, e - b);
}

bool startsWithNoCase(const std::string& s, const char* prefix) {
    std::size_t i = 0;
    for (; prefix[i] != '\0'; ++i) {
        if (i >= s.size()) return false;
        const char a = static_cast<char>(tolower(static_cast<unsigned char>(s[i])));
        const char b = static_cast<char>(tolower(static_cast<unsigned char>(prefix[i])));
        if (a != b) return false;
    }
    return true;
}

// カンマ区切り。maxParts まで分割し、残りは最後の要素にまとめる (テキスト中のカンマ対策)
std::vector<std::string> splitN(const std::string& s, char sep, std::size_t maxParts) {
    std::vector<std::string> out;
    if (maxParts == 0) return out;
    std::size_t pos = 0;
    while (out.size() + 1 < maxParts) {
        const std::size_t p = s.find(sep, pos);
        if (p == std::string::npos) break;
        out.push_back(s.substr(pos, p - pos));
        pos = p + 1;
    }
    out.push_back(s.substr(pos));
    return out;
}

// ASS のタイムコード "H:MM:SS.cc" / SRT の "HH:MM:SS,mmm" を秒へ
bool parseTime(const std::string& raw, double& out) {
    std::string s;
    for (char c : raw) {
        if (!std::isspace(static_cast<unsigned char>(c))) s += c;
    }
    const std::size_t c1 = s.find(':');
    if (c1 == std::string::npos) return false;
    const std::size_t c2 = s.find(':', c1 + 1);
    if (c2 == std::string::npos) return false;

    const std::string hh = s.substr(0, c1);
    const std::string mm = s.substr(c1 + 1, c2 - c1 - 1);
    const std::string ss = s.substr(c2 + 1);
    if (!allDigits(hh) || !allDigits(mm)) return false;

    std::string secPart = ss;
    std::string fracPart;
    const std::size_t dot = ss.find_first_of(",.");
    if (dot != std::string::npos) {
        secPart = ss.substr(0, dot);
        fracPart = ss.substr(dot + 1);
    }
    if (!allDigits(secPart)) return false;
    if (!fracPart.empty() && !allDigits(fracPart)) return false;

    double fraction = 0.0;
    if (!fracPart.empty()) {
        fraction = std::strtod(("0." + fracPart).c_str(), nullptr);
    }
    const long hours = std::strtol(hh.c_str(), nullptr, 10);
    const long minutes = std::strtol(mm.c_str(), nullptr, 10);
    const long seconds = std::strtol(secPart.c_str(), nullptr, 10);
    out = static_cast<double>(hours) * 3600.0 + static_cast<double>(minutes) * 60.0 +
          static_cast<double>(seconds) + fraction;
    return true;
}

// ASS オーバーライドタグ {..} を除去し、\N \n を改行、\h を空白へ置換する。
// <i> 等の SRT タグも除去する (タグの中身は残す)。
std::string sanitizeText(const std::string& in) {
    std::string out;
    out.reserve(in.size());
    for (std::size_t i = 0; i < in.size(); ++i) {
        const char c = in[i];
        if (c == '{') {
            const std::size_t close = in.find('}', i);
            if (close != std::string::npos) {
                i = close;
                continue;
            }
            continue; // 閉じ括弧無し: 以降を捨てる
        }
        if (c == '<') {
            const std::size_t close = in.find('>', i);
            if (close != std::string::npos && close - i <= 16) {
                i = close;
                continue;
            }
        }
        if (c == '\\' && i + 1 < in.size()) {
            const char n = in[i + 1];
            if (n == 'N' || n == 'n') {
                out += '\n';
                ++i;
                continue;
            }
            if (n == 'h') {
                out += ' ';
                ++i;
                continue;
            }
            if (n == '{' || n == '}') {
                out += n;
                ++i;
                continue;
            }
        }
        out += c;
    }
    return trimCopy(out);
}

// ---------------------------------------------------------------------------
//  ファイル読み込み (UTF-8 / UTF-16 BOM / 素の UTF-16LE を UTF-8 へ正規化)
// ---------------------------------------------------------------------------
bool readFileUtf8(const std::wstring& path, std::string& out) {
    std::ifstream in(path, std::ios::binary);
    if (!in) return false;
    const std::string bytes((std::istreambuf_iterator<char>(in)),
                            std::istreambuf_iterator<char>());
    if (bytes.empty()) {
        out.clear();
        return true;
    }

    const auto b0 = static_cast<unsigned char>(bytes[0]);
    const auto b1 = bytes.size() > 1 ? static_cast<unsigned char>(bytes[1]) : 0;

    // UTF-16LE BOM
    if (b0 == 0xFF && b1 == 0xFE) {
        std::wstring wide;
        wide.reserve((bytes.size() - 2) / 2);
        for (std::size_t i = 2; i + 1 < bytes.size(); i += 2) {
            wide += static_cast<wchar_t>(static_cast<unsigned char>(bytes[i]) |
                                         (static_cast<unsigned char>(bytes[i + 1]) << 8));
        }
        out = utf8FromWide(wide);
        return true;
    }
    // UTF-16BE BOM
    if (b0 == 0xFE && b1 == 0xFF) {
        std::wstring wide;
        wide.reserve((bytes.size() - 2) / 2);
        for (std::size_t i = 2; i + 1 < bytes.size(); i += 2) {
            wide += static_cast<wchar_t>((static_cast<unsigned char>(bytes[i]) << 8) |
                                         static_cast<unsigned char>(bytes[i + 1]));
        }
        out = utf8FromWide(wide);
        return true;
    }
    // BOM 無しの UTF-16LE 疑似 (偶数バイトが空いている)
    if (bytes.size() >= 4 && b1 == 0 && static_cast<unsigned char>(bytes[3]) == 0) {
        std::wstring wide;
        wide.reserve(bytes.size() / 2);
        for (std::size_t i = 0; i + 1 < bytes.size(); i += 2) {
            wide += static_cast<wchar_t>(static_cast<unsigned char>(bytes[i]) |
                                         (static_cast<unsigned char>(bytes[i + 1]) << 8));
        }
        out = utf8FromWide(wide);
        return true;
    }

    if (bytes.size() >= 3 && b0 == 0xEF && b1 == 0xBB &&
        static_cast<unsigned char>(bytes[2]) == 0xBF) {
        out = bytes.substr(3);
    } else {
        out = bytes;
    }
    return true;
}

// ---------------------------------------------------------------------------
//  SRT / WebVTT パーサ
// ---------------------------------------------------------------------------
bool parseSrt(const std::string& content, std::vector<SubtitleCue>& cues) {
    std::istringstream ss(content);
    std::vector<std::string> lines;
    std::string line;
    while (std::getline(ss, line)) {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        lines.push_back(line);
    }

    std::size_t i = 0;
    while (i < lines.size()) {
        while (i < lines.size() && trimCopy(lines[i]).empty()) ++i;
        if (i >= lines.size()) break;

        const std::string head = trimCopy(lines[i]);
        if (head.find("-->") == std::string::npos) {
            ++i; // 連番行など
            continue;
        }
        const std::size_t arrow = head.find("-->");
        double start = 0.0;
        double end = 0.0;
        if (!parseTime(head.substr(0, arrow), start) ||
            !parseTime(head.substr(arrow + 3), end)) {
            ++i;
            continue;
        }
        ++i;
        std::string text;
        while (i < lines.size() && !trimCopy(lines[i]).empty()) {
            if (!text.empty()) text += '\n';
            text += lines[i];
            ++i;
        }
        text = sanitizeText(text);
        if (!text.empty()) cues.push_back({start, end, std::move(text)});
    }
    return !cues.empty();
}

// ---------------------------------------------------------------------------
//  ASS / SSA パーサ ([Events] セクションのみ描画対象)
// ---------------------------------------------------------------------------
bool parseAss(const std::string& content, std::vector<SubtitleCue>& cues) {
    std::istringstream ss(content);
    std::string line;
    bool inEvents = false;
    std::size_t idxStart = 1;
    std::size_t idxEnd = 2;
    std::size_t idxText = 9;

    while (std::getline(ss, line)) {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        if (line.empty()) continue;

        if (line.front() == '[') {
            inEvents = startsWithNoCase(line, "[Events]");
            continue;
        }
        if (!inEvents) continue;

        if (startsWithNoCase(line, "Format:")) {
            const std::vector<std::string> fields = splitN(trimCopy(line.substr(7)), ',', 32);
            for (std::size_t k = 0; k < fields.size(); ++k) {
                const std::string name = trimCopy(fields[k]);
                if (startsWithNoCase(name, "Start")) idxStart = k;
                else if (startsWithNoCase(name, "End")) idxEnd = k;
                else if (startsWithNoCase(name, "Text")) idxText = k;
            }
            continue;
        }
        if (!startsWithNoCase(line, "Dialogue:")) continue;

        const std::string payload = trimCopy(line.substr(9));
        const std::size_t maxParts = std::max<std::size_t>(idxText + 1, 4);
        const std::vector<std::string> fields = splitN(payload, ',', maxParts);
        if (fields.size() <= idxStart || fields.size() <= idxEnd) continue;

        double start = 0.0;
        double end = 0.0;
        if (!parseTime(fields[idxStart], start) || !parseTime(fields[idxEnd], end)) continue;
        if (fields.size() <= idxText) continue;

        std::string text = sanitizeText(fields[idxText]);
        if (!text.empty()) cues.push_back({start, end, std::move(text)});
    }
    return !cues.empty();
}

bool looksLikeAss(const std::string& content, const std::string& ext) {
    if (ext == "ass" || ext == "ssa") return true;
    if (ext == "srt" || ext == "vtt") return false;
    return content.find("Dialogue:") != std::string::npos ||
           content.find("[Events]") != std::string::npos;
}

} // namespace

// ---------------------------------------------------------------------------
//  実装
// ---------------------------------------------------------------------------
struct SubtitleSource::Impl {
    mutable std::mutex mutex;
    std::vector<SubtitleCue> cues; // start の昇順 (テキストの一致で重複除去)
    bool fileLoaded = false;       // サイドカー優先フラグ
};

SubtitleSource::SubtitleSource() : m_impl(std::make_unique<Impl>()) {}

SubtitleSource::~SubtitleSource() = default;
SubtitleSource::SubtitleSource(SubtitleSource&&) noexcept = default;
SubtitleSource& SubtitleSource::operator=(SubtitleSource&&) noexcept = default;

bool SubtitleSource::loadFile(const std::wstring& path) {
    std::string content;
    if (!readFileUtf8(path, content)) {
        log::info("SubtitleSource: 読み込み失敗 '%s'", utf8FromWide(path).c_str());
        return false;
    }

    const std::string ext = toLowerExt(path);
    std::vector<SubtitleCue> cues;
    if (looksLikeAss(content, ext)) {
        parseAss(content, cues);
    }
    if (cues.empty()) {
        parseSrt(content, cues);
    }
    if (cues.empty()) {
        parseAss(content, cues);
    }
    if (cues.empty()) {
        log::info("SubtitleSource: 字幕 cue が見つかりません '%s'",
                  utf8FromWide(path).c_str());
        return false;
    }

    std::sort(cues.begin(), cues.end(),
              [](const SubtitleCue& a, const SubtitleCue& b) { return a.start < b.start; });
    cues.erase(std::unique(cues.begin(), cues.end(),
                           [](const SubtitleCue& a, const SubtitleCue& b) {
                               return a.start == b.start && a.text == b.text;
                           }),
               cues.end());

    {
        std::lock_guard<std::mutex> lock(m_impl->mutex);
        m_impl->cues = std::move(cues);
        m_impl->fileLoaded = true;
    }
    log::info("SubtitleSource: %zu cue をロード '%s'", count(),
              utf8FromWide(path).c_str());
    return true;
}

void SubtitleSource::add(double start, double end, std::string text) {
    if (text.empty()) return;
    if (!(end > start)) end = start + 1.0;

    std::lock_guard<std::mutex> lock(m_impl->mutex);
    if (m_impl->fileLoaded) return; // サイドカー優先
    auto& cues = m_impl->cues;
    // start が同じでテキストも同じ cue は重複 (シーク後の再デコード) として捨てる
    auto it = std::lower_bound(cues.begin(), cues.end(), start,
                               [](const SubtitleCue& c, double v) { return c.start < v; });
    if (it != cues.end() && it->start == start && it->text == text) return;
    if (it != cues.begin()) {
        auto prev = it;
        --prev;
        if (prev->start == start && prev->text == text) return;
    }
    cues.insert(it, SubtitleCue{start, end, std::move(text)});
}

void SubtitleSource::clear() {
    std::lock_guard<std::mutex> lock(m_impl->mutex);
    m_impl->cues.clear();
    m_impl->fileLoaded = false;
}

std::string SubtitleSource::textAt(double seconds) const {
    std::lock_guard<std::mutex> lock(m_impl->mutex);
    const auto& cues = m_impl->cues;
    if (cues.empty()) return {};

    // start > seconds の最初の要素
    auto it = std::upper_bound(cues.begin(), cues.end(), seconds,
                               [](double v, const SubtitleCue& c) { return v < c.start; });
    // その直前から遡って表示対象を探す (cue は非重なり想定だが重なりにも耐える)
    for (auto i = it; i != cues.begin();) {
        --i;
        if (i->start <= seconds && seconds < i->end) return i->text;
    }
    return {};
}

std::size_t SubtitleSource::count() const {
    std::lock_guard<std::mutex> lock(m_impl->mutex);
    return m_impl->cues.size();
}

bool SubtitleSource::hasFileCues() const {
    std::lock_guard<std::mutex> lock(m_impl->mutex);
    return m_impl->fileLoaded && !m_impl->cues.empty();
}

bool SubtitleSource::empty() const { return count() == 0; }

bool SubtitleSource::isSubtitlePath(const std::wstring& path) {
    const std::string ext = toLowerExt(path);
    return ext == "srt" || ext == "ass" || ext == "ssa" || ext == "vtt";
}

std::string SubtitleSource::cueText(const char* text, const char* ass) {
    if (text != nullptr && *text != '\0') {
        return sanitizeText(text);
    }
    if (ass == nullptr || *ass == '\0') return {};

    std::string s = ass;
    // "Dialogue: " プレフィックス
    if (startsWithNoCase(s, "Dialogue:")) s = trimCopy(s.substr(9));

    // ASS のイベント行から先頭のメタデータフィールドを取り除く
    const std::vector<std::string> fields = splitN(s, ',', 16);
    std::size_t textIndex = 0;
    double dummyStart = 0.0;
    double dummyEnd = 0.0;
    if (fields.size() >= 10 && parseTime(fields[1], dummyStart) &&
        parseTime(fields[2], dummyEnd)) {
        textIndex = 9; // Layer,Start,End,Style,Name,ML,MR,MV,Effect,Text
    } else if (fields.size() >= 9 && allDigits(trimCopy(fields[0])) &&
               allDigits(trimCopy(fields[1]))) {
        // Matroska 形式: ReadOrder,Layer,Style,Name,ML,MR,MV,Effect,Text
        textIndex = 8;
    }
    std::string joined;
    for (std::size_t i = textIndex; i < fields.size(); ++i) {
        if (i > textIndex) joined += ',';
        joined += fields[i];
    }
    return sanitizeText(joined);
}

} // namespace nanoalpha
