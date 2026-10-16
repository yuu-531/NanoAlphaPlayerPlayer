// =============================================================================
//  OpenRGB SDK クライアント (src/Extensions/OpenRgbClient.cpp)
// -----------------------------------------------------------------------------
//  OpenRGB SDK サーバー (127.0.0.1:6742) へ接続し、
//    ハンドシェイク → デバイス列挙 → LED 数の二分探索 → 色更新
//  を実 TCP (Winsock2 のみ) で行う。
//
//  [パケット]
//   16 バイトのリトルエンディアンヘッダ (magic "ORGB" + dev_id + pkt_id + size)
//   + ペイロード。ペイロードが 0 のパケットも pkt_size = 0 で正しく送る。
//   protocol >= 6 では pkt_dev_id は「コントローラ数応答に含まれる実デバイス ID」
//   (index_from_id で照合される)。それ未満は index になる。
//
//  [受信]
//   サーバーは SIGNALUPDATE (1150) / DEVICE_LIST_UPDATED (100) / ServerFlags (53)
//   などの非同期パケットを混ぜて送ってくるため、「期待している ID の返答 (または
//   ACK) が来るまで読み捨てる」ループで受信する。ヘッダ 16 バイトと payload を
//   不足分までループして読み切り (recvExact)、select によるタイムアウトで
//   WSAETIMEDOUT / WSAECONNRESET / 0 バイト (切断) を日本語エラーにする。
//
//  [LED 数の特定]
//   RGBController::SetColorDescription (resize = false 既定) は num_colors が実際の
//   カラーバッファ長を超えると NET_PACKET_STATUS_ERROR_INVALID_DATA (5) の ACK を
//   返して何も更新しない。これを判定に UPDATELEDS を二分探索する。
//   ACK は OpenRGB 1.0 以降で NET_PACKET_ID_ACK = 10 (仕様メモの 90 も併せて
//   受理)、タイムアウトは 500ms。
//
//  [Winsock]
//   WSAStartup / WSACleanup は静的リファレンスカウントでプロセス内に 1 度だけ。
//   ソケットは non-blocking のまま扱い、必ず select でタイムアウトを挟む。
// =============================================================================
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include "Extensions/OpenRgbClient.hpp"

#include "Core/Log.hpp"

#include <ws2tcpip.h>   // getaddrinfo (winsock2.h より後ろに置く)

#include <bit>
#include <cstring>
#include <exception>
#include <utility>

#pragma comment(lib, "ws2_32.lib")

namespace nanoalpha::rgb {
namespace {

// ---------------------------------------------------------------------------
// パケット ID (OpenRGB NetworkProtocol.h)
// ---------------------------------------------------------------------------
constexpr std::uint32_t kPacketRequestControllerCount = 0;
constexpr std::uint32_t kPacketRequestControllerData   = 1;
constexpr std::uint32_t kPacketProtocolVersion         = 40;
constexpr std::uint32_t kPacketSetClientName           = 50;
constexpr std::uint32_t kPacketSetClientFlags          = 52;
constexpr std::uint32_t kPacketAck                     = 10;  // NET_PACKET_ID_ACK (OpenRGB 1.0 / master)
constexpr std::uint32_t kPacketAckAlternate            = 90;  // 仕様メモ記載値 (互換のため両方受理)
constexpr std::uint32_t kPacketUpdateLeds              = 1050;
constexpr std::uint32_t kPacketSetCustomMode           = 1100;
// 1150 (SIGNALUPDATE) や 100 (DEVICE_LIST_UPDATED) は通知なので応答として
// 待たず、recvPacket / wait* の読み捨てループで破棄する。

// ---------------------------------------------------------------------------
// ACK ステータス (NetPacketStatus)
// ---------------------------------------------------------------------------
constexpr std::uint32_t kStatusOk         = 0;  // NET_PACKET_STATUS_OK
constexpr std::uint32_t kStatusInvalidData = 5;  // NET_PACKET_STATUS_ERROR_INVALID_DATA

// ---------------------------------------------------------------------------
// プロトコル定数
// ---------------------------------------------------------------------------
constexpr std::uint32_t kClientProtocolVersion = 6;      // OPENRGB_SDK_PROTOCOL_VERSION
constexpr std::uint32_t kClientFlags = 1;                // NET_CLIENT_FLAG_SUPPORTS_RGBCONTROLLER
constexpr char kClientName[] = "NanoAlphaPlayer";           // + NUL で 13 バイト
constexpr char kMagic[4] = { 'O', 'R', 'G', 'B' };

// ---------------------------------------------------------------------------
// 予算・上限
// ---------------------------------------------------------------------------
constexpr std::uint32_t kMaxPacketSize = 8u * 1024u * 1024u;  // OpenRGB と同じ 8MB
constexpr unsigned kMaxDevices = 1024;
constexpr unsigned kMaxProbeLedCount = 65535;                  // num_colors (uint16) の最大値
constexpr std::chrono::milliseconds kMinHandshakeTimeout{ 1000 };
constexpr std::chrono::milliseconds kResponseTimeout{ 1000 };
constexpr std::chrono::milliseconds kAckTimeout{ 500 };
constexpr std::chrono::milliseconds kSendTimeout{ 1000 };
constexpr std::chrono::milliseconds kMinSetAllInterval{ 200 };
constexpr int kChunkSize = 64 * 1024;
constexpr long long kMaxSelectMicroseconds = 300LL * 1000LL * 1000LL;  // select 待ち上限 (5 分)

// ---------------------------------------------------------------------------
// バイト列ヘルパ (Windows はリトルエンディアン前提。sendPacket() で static_assert)
// ---------------------------------------------------------------------------
std::uint32_t readU32(const unsigned char* data) noexcept {
    std::uint32_t value = 0;
    std::memcpy(&value, data, sizeof(value));
    return value;
}

std::uint16_t readU16(const unsigned char* data) noexcept {
    std::uint16_t value = 0;
    std::memcpy(&value, data, sizeof(value));
    return value;
}

// 0x00RRGGBB (RGBColor と同じ形式。バイト列は [B, G, R, 0x00])
std::uint32_t makeColor(unsigned char r, unsigned char g, unsigned char b) noexcept {
    return (static_cast<std::uint32_t>(r) << 16) |
           (static_cast<std::uint32_t>(g) << 8) |
            static_cast<std::uint32_t>(b);
}

// 期限までの残り時間 (期限切れは 0)
std::chrono::milliseconds remainingUntil(std::chrono::steady_clock::time_point deadline) noexcept {
    const auto now = std::chrono::steady_clock::now();
    if (now >= deadline) return std::chrono::milliseconds(0);
    return std::chrono::duration_cast<std::chrono::milliseconds>(deadline - now) +
           std::chrono::milliseconds(1);
}

// ---------------------------------------------------------------------------
// Winsock ライフサイクル (プロセス内で 1 度だけ WSAStartup / 対で WSACleanup)
// ---------------------------------------------------------------------------
struct WinsockState {
    std::mutex mutex;
    int refCount = 0;
};

WinsockState& winsockState() noexcept {
    static WinsockState state;
    return state;
}

bool winsockAcquire() noexcept {
    WinsockState& state = winsockState();
    const std::lock_guard<std::mutex> lock(state.mutex);
    if (state.refCount == 0) {
        WSADATA data{};
        if (::WSAStartup(MAKEWORD(2, 2), &data) != 0) return false;
    }
    ++state.refCount;
    return true;
}

void winsockRelease() noexcept {
    WinsockState& state = winsockState();
    const std::lock_guard<std::mutex> lock(state.mutex);
    if (state.refCount > 0) {
        --state.refCount;
        if (state.refCount == 0) {
            ::WSACleanup();
        }
    }
}

} // namespace

// ---------------------------------------------------------------------------
// 基本構成
// ---------------------------------------------------------------------------
Client::Client() noexcept = default;

Client::~Client() noexcept {
    disconnect();
}

void Client::closeSocketLocked() noexcept {
    if (sock_ != INVALID_SOCKET) {
        ::closesocket(sock_);
        sock_ = INVALID_SOCKET;
    }
}

void Client::disconnectLocked() noexcept {
    closeSocketLocked();
    if (wsaActive_) {
        winsockRelease();
        wsaActive_ = false;
    }
    devices_.clear();
    deviceIds_.clear();
    deviceCount_ = 0;
    protocolVersion_ = 0;
    enumerated_ = false;
    hasLastColor_ = false;
    lastSendAtValid_ = false;
}

void Client::disconnect() noexcept {
    const std::lock_guard<std::mutex> lock(mutex_);
    disconnectLocked();
}

bool Client::connected() const noexcept {
    const std::lock_guard<std::mutex> lock(mutex_);
    return sock_ != INVALID_SOCKET;
}

bool Client::devicesEnumerated() const noexcept {
    const std::lock_guard<std::mutex> lock(mutex_);
    return enumerated_;
}

unsigned Client::protocolVersion() const noexcept {
    const std::lock_guard<std::mutex> lock(mutex_);
    return protocolVersion_;
}

unsigned Client::deviceCount() const noexcept {
    const std::lock_guard<std::mutex> lock(mutex_);
    return deviceCount_;
}

unsigned Client::ledCount(unsigned deviceIndex) const noexcept {
    const std::lock_guard<std::mutex> lock(mutex_);
    if (static_cast<std::size_t>(deviceIndex) < devices_.size()) {
        return devices_[deviceIndex].ledCount;
    }
    return 0;
}

const std::string& Client::deviceName(unsigned deviceIndex) const noexcept {
    const std::lock_guard<std::mutex> lock(mutex_);
    if (static_cast<std::size_t>(deviceIndex) < devices_.size()) {
        return devices_[deviceIndex].name;
    }
    return emptyName_;
}

std::uint32_t Client::devIdOf(unsigned deviceIndex) const noexcept {
    if (static_cast<std::size_t>(deviceIndex) < deviceIds_.size()) {
        return deviceIds_[deviceIndex];
    }
    return static_cast<std::uint32_t>(deviceIndex);  // ID 一覧が無い場合は index
}

// ---------------------------------------------------------------------------
// 低層 I/O
// ---------------------------------------------------------------------------
bool Client::waitForSocket(bool forWrite, std::chrono::steady_clock::time_point deadline,
                           std::string& error) {
    const auto now = std::chrono::steady_clock::now();
    if (now >= deadline) {
        error = forWrite ? "送信待ちがタイムアウトしました" : "受信待ちがタイムアウトしました";
        return false;
    }

    long long microseconds =
        std::chrono::duration_cast<std::chrono::microseconds>(deadline - now).count();
    if (microseconds > kMaxSelectMicroseconds) microseconds = kMaxSelectMicroseconds;
    if (microseconds < 1) microseconds = 1;

    timeval timeoutValue{};
    timeoutValue.tv_sec = static_cast<long>(microseconds / 1000000);
    timeoutValue.tv_usec = static_cast<long>(microseconds % 1000000);

    fd_set waitSet;
    FD_ZERO(&waitSet);
    FD_SET(sock_, &waitSet);

    const int result = forWrite
        ? ::select(0, nullptr, &waitSet, nullptr, &timeoutValue)
        : ::select(0, &waitSet, nullptr, nullptr, &timeoutValue);

    if (result == SOCKET_ERROR) {
        error = std::string("select に失敗しました (WSA error ") +
                std::to_string(WSAGetLastError()) + ")";
        return false;
    }
    if (result == 0) {
        error = forWrite ? "送信待ちがタイムアウトしました" : "受信待ちがタイムアウトしました";
        return false;
    }
    return true;
}

bool Client::sendAll(const void* data, std::size_t bytes,
                     std::chrono::steady_clock::time_point deadline, std::string& error) {
    const auto* cursor = static_cast<const unsigned char*>(data);
    std::size_t sent = 0;

    while (sent < bytes) {
        const std::size_t remainingBytes = bytes - sent;
        const int chunk = static_cast<int>(
            remainingBytes > static_cast<std::size_t>(kChunkSize)
                ? static_cast<std::size_t>(kChunkSize)
                : remainingBytes);

        const int result = ::send(sock_, reinterpret_cast<const char*>(cursor + sent), chunk, 0);
        if (result > 0) {
            sent += static_cast<std::size_t>(result);
            continue;
        }
        if (result == 0) {
            error = "送信が 0 バイトで終了しました";
            return false;
        }

        const int socketError = WSAGetLastError();
        if (socketError == WSAEWOULDBLOCK || socketError == WSAEINTR) {
            if (!waitForSocket(true, deadline, error)) return false;
            continue;
        }
        if (socketError == WSAETIMEDOUT) {
            error = "送信がタイムアウトしました (" + std::to_string(sent) + "/" +
                    std::to_string(bytes) + " バイト)";
            return false;
        }
        error = "送信に失敗しました (WSA error " + std::to_string(socketError) + ")";
        return false;
    }
    return true;
}

bool Client::recvExact(void* buffer, std::size_t bytes,
                       std::chrono::steady_clock::time_point deadline, std::string& error) {
    auto* cursor = static_cast<unsigned char*>(buffer);
    std::size_t received = 0;

    while (received < bytes) {
        const std::size_t remainingBytes = bytes - received;
        const int chunk = static_cast<int>(
            remainingBytes > static_cast<std::size_t>(kChunkSize)
                ? static_cast<std::size_t>(kChunkSize)
                : remainingBytes);

        const int result = ::recv(sock_, reinterpret_cast<char*>(cursor + received), chunk, 0);
        if (result > 0) {
            received += static_cast<std::size_t>(result);
            continue;
        }
        if (result == 0) {
            error = "サーバーが接続を閉じました (" + std::to_string(received) + "/" +
                    std::to_string(bytes) + " バイト受信)";
            closeSocketLocked();
            return false;
        }

        const int socketError = WSAGetLastError();
        if (socketError == WSAEWOULDBLOCK || socketError == WSAEINTR) {
            if (!waitForSocket(false, deadline, error)) return false;
            continue;
        }
        if (socketError == WSAETIMEDOUT) {
            error = "受信がタイムアウトしました (" + std::to_string(received) + "/" +
                    std::to_string(bytes) + " バイト)";
            return false;
        }

        error = "受信に失敗しました (WSA error " + std::to_string(socketError) + ")";
        if (socketError == WSAECONNRESET || socketError == WSAECONNABORTED ||
            socketError == WSAENETRESET) {
            closeSocketLocked();
        }
        return false;
    }
    return true;
}

bool Client::sendPacket(std::uint32_t pktId, std::uint32_t devId,
                        const void* payload, std::uint32_t payloadSize, std::string& error) {
    static_assert(sizeof(PacketHeader) == 16, "OpenRGB のパケットヘッダは 16 バイト");
    static_assert(std::endian::native == std::endian::little,
                  "OpenRGB プロトコルはリトルエンディアン前提");

    if (sock_ == INVALID_SOCKET) {
        error = "OpenRGB に接続していません";
        return false;
    }
    if (payloadSize > 0 && payload == nullptr) {
        error = "送信ペイロードが nullptr です";
        return false;
    }

    PacketHeader header{};
    std::memcpy(header.magic, kMagic, sizeof(kMagic));
    header.devId = devId;
    header.pktId = pktId;
    header.pktSize = payloadSize;

    const auto deadline = std::chrono::steady_clock::now() + kSendTimeout;
    if (!sendAll(&header, sizeof(header), deadline, error)) return false;
    if (payloadSize > 0) {
        if (!sendAll(payload, payloadSize, deadline, error)) return false;
    }
    return true;
}

bool Client::recvPacket(PacketHeader& header, std::vector<unsigned char>& payload,
                        std::chrono::milliseconds timeout, std::string& error) {
    payload.clear();

    const auto deadline = std::chrono::steady_clock::now() + timeout;
    if (!recvExact(&header, sizeof(header), deadline, error)) return false;

    if (std::memcmp(header.magic, kMagic, sizeof(kMagic)) != 0) {
        error = "OpenRGB パケットのマジックが不正です";
        closeSocketLocked();  // ストリームが同期不能になったので閉じる
        return false;
    }
    if (header.pktSize > kMaxPacketSize) {
        error = "パケットサイズが上限を超えました (" + std::to_string(header.pktSize) + " バイト)";
        closeSocketLocked();
        return false;
    }

    if (header.pktSize > 0) {
        payload.resize(header.pktSize);  // 途中の部分読みは recvExact が不足分まで回る
        if (!recvExact(payload.data(), payload.size(), deadline, error)) return false;
    }
    return true;
}

bool Client::waitResponse(std::uint32_t expectedPktId, std::vector<unsigned char>& payload,
                          std::chrono::milliseconds timeout, std::string& error) {
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    PacketHeader header{};

    payload.clear();
    for (;;) {
        const auto remaining = remainingUntil(deadline);
        if (remaining.count() <= 0) {
            error = "応答待ちがタイムアウトしました (pkt id " + std::to_string(expectedPktId) + ")";
            return false;
        }
        if (!recvPacket(header, payload, remaining, error)) return false;
        if (header.pktId == expectedPktId) return true;

        // 通知 (SIGNALUPDATE / DEVICE_LIST_UPDATED / ServerFlags 等) や
        // 無関係な ACK は読み捨てる
        payload.clear();
    }
}

bool Client::waitAck(std::uint32_t expectedPktId, std::uint32_t& status,
                     std::chrono::milliseconds timeout, std::string& error) {
    const auto deadline = std::chrono::steady_clock::now() + timeout;

    for (;;) {
        const auto remaining = remainingUntil(deadline);
        if (remaining.count() <= 0) {
            error = "ACK 待ちがタイムアウトしました (pkt id " + std::to_string(expectedPktId) + ")";
            return false;
        }

        PacketHeader header{};
        std::vector<unsigned char> payload;
        if (!recvPacket(header, payload, remaining, error)) return false;

        if (header.pktId != kPacketAck && header.pktId != kPacketAckAlternate) {
            continue;  // 通知パケットは読み捨てる
        }
        if (payload.size() < 8) {
            error = "ACK の応答が短すぎます (" + std::to_string(payload.size()) + " バイト)";
            return false;
        }

        const std::uint32_t ackedPktId = readU32(payload.data());
        if (ackedPktId != expectedPktId) {
            continue;  // 直前の要求に対する ACK などは読み捨てる
        }

        status = readU32(payload.data() + 4);
        return true;
    }
}

// ---------------------------------------------------------------------------
// 接続
// ---------------------------------------------------------------------------
bool Client::connect(const std::string& host, unsigned short port,
                     std::chrono::milliseconds timeout, std::string& error) {
    const std::lock_guard<std::mutex> lock(mutex_);
    error.clear();

    try {
        return connectLocked(host, port, timeout, error);
    } catch (const std::exception& exception) {
        error = std::string("接続中に例外が発生しました (") + exception.what() + ")";
    } catch (...) {
        error = "接続中に不明な例外が発生しました";
    }

    log::info("OpenRGB: %s", error.c_str());
    disconnectLocked();
    return false;
}

bool Client::connectLocked(const std::string& host, unsigned short port,
                           std::chrono::milliseconds timeout, std::string& error) {
    disconnectLocked();  // 前回接続の掃除 (Winsock 参照カウントもここで戻す)

    if (host.empty()) {
        error = "ホスト名が空です";
        log::info("OpenRGB: %s", error.c_str());
        return false;
    }

    if (!winsockAcquire()) {
        error = "WSAStartup (Winsock 2.2) に失敗しました";
        log::info("OpenRGB: %s", error.c_str());
        return false;
    }
    wsaActive_ = true;  // ここからの失敗は disconnectLocked() が解放する

    const auto fail = [this, &error](std::string message) {
        error = std::move(message);
        log::info("OpenRGB: %s", error.c_str());
        disconnectLocked();
        return false;
    };

    // -----------------------------------------------------------------
    // 1. TCP 接続 (non-blocking + select でタイムアウト内に完了)
    // -----------------------------------------------------------------
    addrinfo hints{};
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    hints.ai_protocol = IPPROTO_TCP;

    addrinfo* addresses = nullptr;
    const std::string portText = std::to_string(port);
    const int resolveResult = ::getaddrinfo(host.c_str(), portText.c_str(), &hints, &addresses);
    if (resolveResult != 0 || addresses == nullptr) {
        return fail("ホスト名の解決に失敗しました: " + host + " (getaddrinfo=" +
                    std::to_string(resolveResult) + ")");
    }

    const auto connectBudget = (timeout > std::chrono::milliseconds(0))
        ? timeout
        : std::chrono::milliseconds(1);
    const auto connectDeadline = std::chrono::steady_clock::now() + connectBudget;

    SOCKET socketHandle = INVALID_SOCKET;
    for (addrinfo* address = addresses;
         address != nullptr && socketHandle == INVALID_SOCKET;
         address = address->ai_next) {
        if (std::chrono::steady_clock::now() >= connectDeadline) break;

        SOCKET candidate = ::socket(address->ai_family, address->ai_socktype, address->ai_protocol);
        if (candidate == INVALID_SOCKET) continue;

        u_long nonBlocking = 1;
        if (::ioctlsocket(candidate, FIONBIO, &nonBlocking) != 0) {
            ::closesocket(candidate);
            continue;
        }

        const int connectResult =
            ::connect(candidate, address->ai_addr, static_cast<int>(address->ai_addrlen));
        if (connectResult != 0) {
            const int connectError = WSAGetLastError();
            if (connectError != WSAEWOULDBLOCK && connectError != WSAEINPROGRESS) {
                ::closesocket(candidate);
                continue;
            }

            fd_set writeSet;
            FD_ZERO(&writeSet);
            FD_SET(candidate, &writeSet);

            const auto now = std::chrono::steady_clock::now();
            if (now >= connectDeadline) {
                ::closesocket(candidate);
                break;
            }
            long long microseconds =
                std::chrono::duration_cast<std::chrono::microseconds>(connectDeadline - now).count();
            if (microseconds > kMaxSelectMicroseconds) microseconds = kMaxSelectMicroseconds;
            if (microseconds < 1) microseconds = 1;

            timeval timeoutValue{};
            timeoutValue.tv_sec = static_cast<long>(microseconds / 1000000);
            timeoutValue.tv_usec = static_cast<long>(microseconds % 1000000);

            const int selected = ::select(0, nullptr, &writeSet, nullptr, &timeoutValue);
            if (selected <= 0) {
                ::closesocket(candidate);
                continue;
            }

            int socketError = 0;
            int optionLength = static_cast<int>(sizeof(socketError));
            if (::getsockopt(candidate, SOL_SOCKET, SO_ERROR,
                             reinterpret_cast<char*>(&socketError), &optionLength) != 0 ||
                socketError != 0) {
                ::closesocket(candidate);
                continue;
            }
        }

        socketHandle = candidate;
    }
    ::freeaddrinfo(addresses);

    if (socketHandle == INVALID_SOCKET) {
        return fail("OpenRGB サーバーに接続できません (" + host + ":" + portText + ")");
    }
    sock_ = socketHandle;  // ここから先の失敗は disconnectLocked() が閉じる
    log::info("OpenRGB: %s:%s へ接続しました", host.c_str(), portText.c_str());

    // -----------------------------------------------------------------
    // 2. プロトコルバージョン交換
    // -----------------------------------------------------------------
    const auto handshakeBudget =
        (timeout > kMinHandshakeTimeout) ? timeout : kMinHandshakeTimeout;
    const auto handshakeDeadline = std::chrono::steady_clock::now() + handshakeBudget;

    const std::uint32_t ourProtocol = kClientProtocolVersion;
    if (!sendPacket(kPacketProtocolVersion, 0, &ourProtocol,
                    static_cast<std::uint32_t>(sizeof(ourProtocol)), error)) {
        return fail(error);
    }

    std::vector<unsigned char> payload;
    if (!waitResponse(kPacketProtocolVersion, payload, remainingUntil(handshakeDeadline), error)) {
        return fail("プロトコルバージョン交換: " + error);
    }
    if (payload.size() < 4) {
        return fail("プロトコルバージョンの応答が短すぎます (" +
                    std::to_string(payload.size()) + " バイト)");
    }
    protocolVersion_ = readU32(payload.data());

    if (protocolVersion_ < kClientProtocolVersion) {
        return fail("OpenRGB サーバーのプロトコルが " + std::to_string(protocolVersion_) +
                    " です (" + std::to_string(kClientProtocolVersion) +
                    " 以上が必要です。OpenRGB 1.0 以降を使用してください)");
    }

    // -----------------------------------------------------------------
    // 3. クライアント名 (NUL 終端込み 13 バイト)
    // -----------------------------------------------------------------
    if (!sendPacket(kPacketSetClientName, 0, kClientName,
                    static_cast<std::uint32_t>(sizeof(kClientName)), error)) {
        return fail(error);
    }

    // -----------------------------------------------------------------
    // 4. クライアントフラグ
    // -----------------------------------------------------------------
    const std::uint32_t clientFlags = kClientFlags;
    if (!sendPacket(kPacketSetClientFlags, 0, &clientFlags,
                    static_cast<std::uint32_t>(sizeof(clientFlags)), error)) {
        return fail(error);
    }

    // -----------------------------------------------------------------
    // 5. コントローラ数 (+ protocol >= 6 の実デバイス ID 一覧)
    //    この手順で返る余分な ACK / ServerFlags は次工程の waitResponse が
    //    読み捨てる。
    // -----------------------------------------------------------------
    if (!requestControllerCount(error)) {
        return fail(error);
    }

    log::info("OpenRGB: handshake 完了 (server protocol %u, %u device(s))",
              protocolVersion_, deviceCount_);
    return true;
}

bool Client::requestControllerCount(std::string& error) {
    if (!sendPacket(kPacketRequestControllerCount, 0, nullptr, 0, error)) return false;

    std::vector<unsigned char> payload;
    if (!waitResponse(kPacketRequestControllerCount, payload, kResponseTimeout, error)) {
        return false;
    }
    if (payload.size() < 4) {
        error = "コントローラ数の応答が短すぎます (" + std::to_string(payload.size()) + " バイト)";
        return false;
    }

    const std::uint32_t count = readU32(payload.data());
    if (count > kMaxDevices) {
        error = "コントローラ数が不正です (" + std::to_string(count) + " 台)";
        return false;
    }
    deviceCount_ = count;
    deviceIds_.clear();

    // protocol >= 6 では count x uint32 の実デバイス ID が続く (読み飛ばさず
    // 受け取る。index ではなく ID が pkt_dev_id として必要)。
    const std::size_t expectedSize = 4 + (static_cast<std::size_t>(count) * 4);
    if (payload.size() >= expectedSize) {
        deviceIds_.resize(static_cast<std::size_t>(count));
        for (std::size_t index = 0; index < deviceIds_.size(); ++index) {
            deviceIds_[index] = readU32(payload.data() + 4 + (index * 4));
        }
    } else if (payload.size() > 4) {
        log::info("OpenRGB: コントローラ ID 一覧が不完全です (%zu バイト) — index を使います",
                  payload.size());
    }
    return true;
}

// ---------------------------------------------------------------------------
// デバイス列挙
// ---------------------------------------------------------------------------
bool Client::fetchDeviceName(unsigned deviceIndex, std::string& name, std::string& error) {
    // protocol == 0 のときだけ size = 0。それ以外は uint32 のプロトコルを積む
    const std::uint32_t protocolPayload = protocolVersion_;
    const void* payloadData = &protocolPayload;
    std::uint32_t payloadSize = static_cast<std::uint32_t>(sizeof(protocolPayload));
    if (protocolVersion_ == 0) {
        payloadData = nullptr;
        payloadSize = 0;
    }

    if (!sendPacket(kPacketRequestControllerData, devIdOf(deviceIndex), payloadData,
                    payloadSize, error)) {
        return false;
    }

    std::vector<unsigned char> reply;
    if (!waitResponse(kPacketRequestControllerData, reply, kResponseTimeout, error)) {
        return false;
    }

    // 応答: uint32 reply_size (全体長) + uint32 device_type + uint16 name_len + name
    // (reply_size プレフィックスが無い形式にも対応する)
    std::size_t base = 0;
    if (reply.size() >= 4 && readU32(reply.data()) == static_cast<std::uint32_t>(reply.size())) {
        base = 4;
    }
    const std::size_t nameLengthOffset = base + 4;  // device_type (4 バイト) の後
    const std::size_t nameOffset = nameLengthOffset + 2;
    if (reply.size() < nameOffset) {
        error = "コントローラデータの応答が短すぎます (" + std::to_string(reply.size()) + " バイト)";
        return false;
    }

    const std::uint16_t nameLength = readU16(reply.data() + nameLengthOffset);
    if (nameLength == 0 || (nameOffset + nameLength) > reply.size()) {
        error = "コントローラデータに名前が含まれていません";
        return false;
    }

    std::string parsed;
    parsed.reserve(nameLength);
    for (std::uint16_t index = 0; index < nameLength; ++index) {
        const char character = static_cast<char>(reply[nameOffset + index]);
        if (character == '\0') break;
        if (static_cast<unsigned char>(character) >= 0x20 && character != '\x7f') {
            parsed.push_back(character);
        }
    }
    if (parsed.empty()) {
        error = "コントローラ名が空です";
        return false;
    }

    name = std::move(parsed);
    return true;
}

bool Client::setCustomMode(unsigned deviceIndex, std::string& error) {
    if (!sendPacket(kPacketSetCustomMode, devIdOf(deviceIndex), nullptr, 0, error)) {
        return false;
    }

    std::uint32_t status = kStatusOk;
    if (!waitAck(kPacketSetCustomMode, status, kAckTimeout, error)) return false;

    if (status != kStatusOk) {
        error = "カスタムモード化が拒否されました (device " + std::to_string(deviceIndex) +
                ", status " + std::to_string(status) + ")";
        return false;
    }
    return true;
}

bool Client::updateLeds(unsigned deviceIndex, unsigned ledCount, std::uint32_t color,
                        std::uint32_t& status, std::string& error) {
    status = kStatusOk;
    if (ledCount > kMaxProbeLedCount) {
        error = "LED 数が uint16 の範囲を超えています (" + std::to_string(ledCount) + ")";
        return false;
    }

    // ペイロード: uint32 data_size (自身を含む全体長) + uint16 num_colors + 色 x n
    //   data_size = 4 + 2 + n*4 は pkt_size と一致している必要がある
    const std::uint32_t payloadSize =
        4u + 2u + (static_cast<std::uint32_t>(ledCount) * 4u);
    std::vector<unsigned char> payload(payloadSize);

    std::memcpy(payload.data(), &payloadSize, sizeof(payloadSize));
    const std::uint16_t numColors = static_cast<std::uint16_t>(ledCount);
    std::memcpy(payload.data() + 4, &numColors, sizeof(numColors));
    for (unsigned index = 0; index < ledCount; ++index) {
        std::memcpy(payload.data() + 6 + (static_cast<std::size_t>(index) * 4),
                    &color, sizeof(color));
    }

    if (!sendPacket(kPacketUpdateLeds, devIdOf(deviceIndex), payload.data(), payloadSize, error)) {
        return false;
    }
    return waitAck(kPacketUpdateLeds, status, kAckTimeout, error);
}

bool Client::resolveLedCount(unsigned deviceIndex, std::stop_token stop,
                             unsigned& ledCount, std::string& error) {
    ledCount = 0;

    // 探索中の色は「最後に使った色」(未使用なら白)。成功した probe はその色に
    // なるが、後で setAll() が最終色を送るため問題にならない。
    const std::uint32_t probeColor =
        hasLastColor_ ? makeColor(lastR_, lastG_, lastB_) : 0x00FFFFFFu;

    // lo = 0 は成功が既知 (num_colors = 0 は SetColorDescription が必ず許容する)。
    // hi は uint16 の最大値。仕様メモの 65536 は num_colors に収まらず 0 として
    // 送られてしまう (常に成功してしまう) ため 65535 を上端にする。
    unsigned low = 0;
    unsigned high = kMaxProbeLedCount;

    std::uint32_t status = kStatusOk;
    if (!updateLeds(deviceIndex, high, probeColor, status, error)) return false;

    if (status == kStatusOk) {
        ledCount = high;
        log::info("OpenRGB: device %u は LED 数が上限 (%u) で飽和しました",
                  deviceIndex, high);
        return true;
    }
    if (status != kStatusInvalidData) {
        error = "LED 数の探索で予期しない応答です (device " + std::to_string(deviceIndex) +
                ", status " + std::to_string(status) + ")";
        return false;
    }

    // 二分探索: 成功なら LED 数 >= mid、INVALID_DATA なら LED 数 < mid
    while ((high - low) > 1) {
        if (stop.stop_requested()) {
            error = "停止要求により中断しました";
            return false;
        }

        const unsigned middle = low + ((high - low) / 2u);
        if (!updateLeds(deviceIndex, middle, probeColor, status, error)) return false;

        if (status == kStatusOk) {
            low = middle;
        } else if (status == kStatusInvalidData) {
            high = middle;
        } else {
            error = "LED 数の探索で予期しない応答です (device " +
                    std::to_string(deviceIndex) + ", status " + std::to_string(status) +
                    ", n=" + std::to_string(middle) + ")";
            return false;
        }
    }

    ledCount = low;
    return true;
}

bool Client::enumerate(std::stop_token stop, std::string& error) {
    const std::lock_guard<std::mutex> lock(mutex_);
    error.clear();

    const auto fail = [&error]() {
        log::info("OpenRGB: %s", error.c_str());
        return false;
    };

    if (sock_ == INVALID_SOCKET) {
        error = "OpenRGB に接続していません";
        return fail();
    }
    if (stop.stop_requested()) {
        error = "停止要求により中断しました";
        return fail();
    }

    try {
        // デバイスは接続後にも増減するため、列挙のたびに件数と ID を取り直す
        if (!requestControllerCount(error)) return fail();

        if (deviceCount_ == 0) {
            devices_.clear();
            enumerated_ = true;
            hasLastColor_ = false;
            log::info("OpenRGB: デバイスが見つかりません (0 台)");
            return true;
        }

        std::vector<Device> devices(static_cast<std::size_t>(deviceCount_));

        for (unsigned index = 0; index < deviceCount_; ++index) {
            if (stop.stop_requested()) {
                error = "停止要求により中断しました";
                return fail();
            }

            devices[index].name = "OpenRGB Device " + std::to_string(index);

            // (a) コントローラデータから名前を取得 (失敗しても列挙は続行する)
            std::string nameError;
            if (!fetchDeviceName(index, devices[index].name, nameError)) {
                if (sock_ == INVALID_SOCKET) {
                    error = "デバイス名の取得中に接続が切断されました: " + nameError;
                    return fail();
                }
                log::info("OpenRGB: device %u の名前取得に失敗 (%s) → フォールバック名を使用",
                          index, nameError.c_str());
            }

            // (b) カスタムモード化 (ACK を待つ)
            if (!setCustomMode(index, error)) return fail();

            // (c) LED 数を二分探索で解決
            if (!resolveLedCount(index, stop, devices[index].ledCount, error)) return fail();

            log::info("OpenRGB: device %u '%s' — %u LED",
                      index, devices[index].name.c_str(), devices[index].ledCount);
        }

        devices_ = std::move(devices);
        enumerated_ = true;
        hasLastColor_ = false;
        lastSendAtValid_ = false;
        log::info("OpenRGB: %u 台のデバイスを列挙しました", deviceCount_);
        return true;
    } catch (const std::exception& exception) {
        error = std::string("デバイス列挙中に例外が発生しました (") + exception.what() + ")";
    } catch (...) {
        error = "デバイス列挙中に不明な例外が発生しました";
    }
    return fail();
}

// ---------------------------------------------------------------------------
// 色更新
// ---------------------------------------------------------------------------
bool Client::setAll(unsigned char r, unsigned char g, unsigned char b) {
    const std::lock_guard<std::mutex> lock(mutex_);

    try {
        if (sock_ == INVALID_SOCKET || !enumerated_) return false;

        // 色が前回と全く同じなら何もしない
        if (hasLastColor_ && r == lastR_ && g == lastG_ && b == lastB_) return true;

        // 200ms 以上間隔を空ける (メインループから毎フレーム呼ばれても
        // ネットワークを氾濫させない)。今回は送らず、次回の呼び出しで送る。
        const auto now = std::chrono::steady_clock::now();
        if (lastSendAtValid_ && (now - lastSendAt_) < kMinSetAllInterval) return true;

        const std::uint32_t color = makeColor(r, g, b);
        bool allSucceeded = true;
        bool sentAny = false;
        std::string error;

        for (std::size_t index = 0; index < devices_.size(); ++index) {
            if (devices_[index].ledCount == 0) continue;  // LED なしはスキップ

            const auto deviceIndex = static_cast<unsigned>(index);
            std::uint32_t status = kStatusOk;
            if (!updateLeds(deviceIndex, devices_[index].ledCount, color, status, error)) {
                allSucceeded = false;
                log::info("OpenRGB: device %u の LED 更新に失敗 (%s)",
                          deviceIndex, error.c_str());
                break;  // 接続自体が壊れている可能性が高いので以降は打ち切る
            }

            sentAny = true;
            if (status != kStatusOk) {
                allSucceeded = false;
                log::info("OpenRGB: device %u の LED 更新が拒否されました (status %u)",
                          deviceIndex, status);
            }
        }

        if (sentAny) {
            lastSendAt_ = now;
            lastSendAtValid_ = true;
        }
        if (allSucceeded) {
            lastR_ = r;
            lastG_ = g;
            lastB_ = b;
            hasLastColor_ = true;
        }
        return allSucceeded;
    } catch (const std::exception& exception) {
        log::info("OpenRGB: LED 更新中に例外が発生しました (%s)", exception.what());
    } catch (...) {
        log::info("OpenRGB: LED 更新中に不明な例外が発生しました");
    }
    return false;
}

} // namespace nanoalpha::rgb
