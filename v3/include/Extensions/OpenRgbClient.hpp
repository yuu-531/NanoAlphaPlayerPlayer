// =============================================================================
//  OpenRGB SDK クライアント (include/Extensions/OpenRgbClient.hpp)
// -----------------------------------------------------------------------------
//  OpenRGB の SDK サーバー (127.0.0.1:6742) と "ORGB" バイナリプロトコルで
//  直接 TCP 通信するクライアント。外部ライブラリ / JSON パーサは使わず、
//  Winsock2 と標準ライブラリのみで実装している。
//
//  [接続フロー (NetworkClient::ConnectionThreadFunction と同じ順序)]
//   1. TCP 接続 (select によるタイムアウトつき)
//   2. REQUEST_PROTOCOL_VERSION (40, uint32 6) → サーバーのプロトコルを取得
//   3. SET_CLIENT_NAME (50, "NanoAlphaPlayer\0") / SET_CLIENT_FLAGS (52, uint32 1)
//   4. REQUEST_CONTROLLER_COUNT (0) → デバイス数
//      (protocol >= 6 のときは count x uint32 の「実デバイス ID」一覧が続く)
//   5. enumerate(): デバイスごとに
//      REQUEST_CONTROLLER_DATA (1) で名前を取得
//      → RGBCONTROLLER_SETCUSTOMMODE (1100) + ACK 待ち
//      → RGBCONTROLLER_UPDATELEDS (1050) の二分探索で LED 数を特定
//
//  [LED 数の特定]
//   サーバー側 RGBController::SetColorDescription (resize = false 既定) は
//   num_colors が実際のカラーバッファ長を超えると
//   NET_PACKET_STATUS_ERROR_INVALID_DATA (5) の ACK を返して更新しない。
//   この性質を使い、UPDATELEDS を二分探索 (往復は 17 回程度・各 ACK は 500ms
//   タイムアウト) して LED 数を安全に特定する。
//
//  [スレッド]
//   すべての公開メンバは内部 mutex で保護されスレッドセーフ。
//   ネットワーク I/O は必ずタイムアウトつき (ロックを握る期間は有限) で、
//   例外は呼び出し元へ出さない (nothrow 系)。
// =============================================================================
#pragma once
#ifndef NANOALPHA_EXTENSIONS_OPEN_RGB_CLIENT_HPP
#define NANOALPHA_EXTENSIONS_OPEN_RGB_CLIENT_HPP

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <winsock2.h>   // 付け忘れると windows.h との順序で衝突する。windows.h の後ろに置く

#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <mutex>
#include <stop_token>
#include <string>
#include <vector>

namespace nanoalpha::rgb {

// OpenRGB SDK サーバー (127.0.0.1:6742) への実クライアント。
// 接続・ハンドシェイク・デバイス列挙・LED 色の更新を実 TCP で行う。
// すべての公開メンバはスレッドセーフ (内部に mutex を持つ)。
class Client {
public:
    Client() noexcept;
    ~Client() noexcept;
    Client(const Client&) = delete;
    Client& operator=(const Client&) = delete;

    // 接続 (タイムアウトつき)。成功すれば既にハンドシェイクまで完了している。
    // OpenRGB 未起動・プロトコル不足などは例外を投げず false + error (日本語)。
    bool connect(const std::string& host, unsigned short port,
                 std::chrono::milliseconds timeout, std::string& error);

    void disconnect() noexcept;
    bool connected() const noexcept;
    bool devicesEnumerated() const noexcept;
    unsigned protocolVersion() const noexcept;
    unsigned deviceCount() const noexcept;
    unsigned ledCount(unsigned deviceIndex) const noexcept;  // 未取得なら 0
    // デバイス名。範囲外 / 未取得なら空文字列。
    // 返される参照は次に enumerate() / disconnect() が呼ばれるまで有効。
    const std::string& deviceName(unsigned deviceIndex) const noexcept;

    // デバイス列挙 + カスタムモード化 + LED 数の解決を行う (ローダ呼び出し専用)。
    // stop で中断可能。false なら error に理由。
    bool enumerate(std::stop_token stop, std::string& error);

    // 全デバイスの LED を同じ色で更新する (低頻度で呼ぶ。内部で 200ms 以上間隔を空ける)。
    // 色が前回と同一なら何もせず true。間隔不足で今回送らない場合も true
    // (次回の呼び出しで送信される)。false は通信エラー (ログにも残る)。
    bool setAll(unsigned char r, unsigned char g, unsigned char b);

private:
    struct Device {
        std::string name;
        unsigned ledCount = 0;
    };

    // OpenRGB のパケットヘッダ (16 バイト・リトルエンディアン)
    struct PacketHeader {
        char magic[4];
        std::uint32_t devId;
        std::uint32_t pktId;
        std::uint32_t pktSize;
    };

    // ---- 接続・低層 I/O (mutex_ を保持した状態で呼ぶ) ----
    bool connectLocked(const std::string& host, unsigned short port,
                       std::chrono::milliseconds timeout, std::string& error);
    bool sendPacket(std::uint32_t pktId, std::uint32_t devId,
                    const void* payload, std::uint32_t payloadSize, std::string& error);
    bool sendAll(const void* data, std::size_t bytes,
                 std::chrono::steady_clock::time_point deadline, std::string& error);
    bool recvExact(void* buffer, std::size_t bytes,
                   std::chrono::steady_clock::time_point deadline, std::string& error);
    bool recvPacket(PacketHeader& header, std::vector<unsigned char>& payload,
                    std::chrono::milliseconds timeout, std::string& error);
    bool waitForSocket(bool forWrite, std::chrono::steady_clock::time_point deadline,
                       std::string& error);
    // 期待した ID の返答が来るまで、通知パケット (SIGNALUPDATE 等) を読み捨てる。
    bool waitResponse(std::uint32_t expectedPktId, std::vector<unsigned char>& payload,
                      std::chrono::milliseconds timeout, std::string& error);
    // acked_pkt_id が一致する ACK が来るまで読み捨てる (status を返す)。
    bool waitAck(std::uint32_t expectedPktId, std::uint32_t& status,
                 std::chrono::milliseconds timeout, std::string& error);

    // ---- プロトコル処理 (mutex_ を保持した状態で呼ぶ) ----
    std::uint32_t devIdOf(unsigned deviceIndex) const noexcept;
    bool requestControllerCount(std::string& error);
    bool fetchDeviceName(unsigned deviceIndex, std::string& name, std::string& error);
    bool setCustomMode(unsigned deviceIndex, std::string& error);
    // UPDATELEDS を送り、ACK の status を返す (送信失敗のみ false)。
    bool updateLeds(unsigned deviceIndex, unsigned ledCount, std::uint32_t color,
                    std::uint32_t& status, std::string& error);
    bool resolveLedCount(unsigned deviceIndex, std::stop_token stop,
                         unsigned& ledCount, std::string& error);
    void closeSocketLocked() noexcept;
    void disconnectLocked() noexcept;

    mutable std::mutex mutex_;
    SOCKET sock_ = INVALID_SOCKET;
    bool wsaActive_ = false;
    bool enumerated_ = false;
    unsigned protocolVersion_ = 0;
    unsigned deviceCount_ = 0;
    std::vector<Device> devices_;
    std::vector<std::uint32_t> deviceIds_;  // protocol >= 6 の実 ID (無ければ index)
    std::string emptyName_;                 // deviceName() の範囲外フォールバック
    bool hasLastColor_ = false;
    unsigned char lastR_ = 0;
    unsigned char lastG_ = 0;
    unsigned char lastB_ = 0;
    bool lastSendAtValid_ = false;
    std::chrono::steady_clock::time_point lastSendAt_{};
};

} // namespace nanoalpha::rgb

#endif // NANOALPHA_EXTENSIONS_OPEN_RGB_CLIENT_HPP
