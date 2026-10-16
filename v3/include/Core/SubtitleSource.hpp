// =============================================================================
//  字幕キューストア (include/Core/SubtitleSource.hpp)
// -----------------------------------------------------------------------------
//  ・SRT / ASS / SSA のサイドカー字幕ファイルを全件ロードする
//  ・埋め込み字幕ストリームの cue をデコードスレッドから追加できる
//  ・head から FFmpeg を見せない (pimpl)。テキストは UTF-8。
//  ・メインスレッド (表示) / デコードスレッド (追加) から同時に使うため
//    内部で 1 本のミューテキスで保護する。
// =============================================================================
#ifndef NANOALPHA_CORE_SUBTITLE_SOURCE_HPP
#define NANOALPHA_CORE_SUBTITLE_SOURCE_HPP

#include <cstddef>
#include <memory>
#include <string>
#include <vector>

namespace nanoalpha {

// 字幕 cue: [start, end) に表示する UTF-8 テキスト
struct SubtitleCue {
    double start = 0.0;
    double end = 0.0;
    std::string text;
};

class SubtitleSource {
public:
    SubtitleSource();
    ~SubtitleSource();
    SubtitleSource(SubtitleSource&&) noexcept;
    SubtitleSource& operator=(SubtitleSource&&) noexcept;
    SubtitleSource(const SubtitleSource&) = delete;
    SubtitleSource& operator=(const SubtitleSource&) = delete;

    // サイドカー字幕ファイルを全件ロードする (既存キューは置き換える)。
    // 成功時 true。拡張子から SRT / ASS / SSA を判別する。
    bool loadFile(const std::wstring& path);

    // 埋め込み字幕 (デコードスレッド) から cue を受け取る。時系列順に挿入される。
    void add(double start, double end, std::string text);

    // 全 cue を破棄する。
    void clear();

    // seconds に表示すべき cue のテキスト (UTF-8)。無ければ空。
    std::string textAt(double seconds) const;

    std::size_t count() const;
    bool empty() const;

    // サイドカー (loadFile) がロード済みか。 true の間 add() は無視される。
    bool hasFileCues() const;

    // 拡張子から字幕ファイルと判定する (.srt / .ass / .ssa / .vtt)
    static bool isSubtitlePath(const std::wstring& path);

    // ASS のダイアログ行から表示用テキストを取り出す (タグ除去・\N 改行化)。
    // text が有効ならそれを優先する。
    static std::string cueText(const char* text, const char* ass);

private:
    struct Impl;
    std::unique_ptr<Impl> m_impl;
};

} // namespace nanoalpha

#endif // NANOALPHA_CORE_SUBTITLE_SOURCE_HPP
