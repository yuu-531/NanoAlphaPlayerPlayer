#include "Extensions/ExtensionManager.hpp"

#include "Core/Log.hpp"

#include <algorithm>
#include <exception>
#include <future>
#include <utility>

namespace nanoalpha {

// ---------------------------------------------------------------------------
// 文字列化
// ---------------------------------------------------------------------------

std::string_view toString(ExtensionId id) noexcept {
    switch (id) {
    case ExtensionId::FrameGeneration: return "Frame Generation";
    case ExtensionId::OpenRgb:         return "OpenRGB Sync";
    case ExtensionId::Disc:            return "CD-DA / DVD";
    case ExtensionId::NetworkStream:   return "Network Streaming";
    case ExtensionId::VlcFeatures:     return "VLC Features";
    case ExtensionId::Count:           break;
    }
    return "Unknown";
}

std::string_view toString(ExtensionState state) noexcept {
    switch (state) {
    case ExtensionState::Pending:   return "Pending";
    case ExtensionState::Loading:   return "Loading";
    case ExtensionState::Ready:     return "Ready";
    case ExtensionState::Failed:    return "Failed";
    case ExtensionState::TimedOut:  return "TimedOut";
    case ExtensionState::Cancelled: return "Cancelled";
    }
    return "Unknown";
}

// ---------------------------------------------------------------------------
// 登録 / 起動 / 停止
// ---------------------------------------------------------------------------

bool ExtensionManager::add(ExtensionId id, LoadPriority priority, LoaderFn loader, std::string name) {
    if (!loader) return false;

    const auto index = static_cast<std::size_t>(id);
    if (index >= static_cast<std::size_t>(ExtensionId::Count)) return false;
    if (m_slots.size() <= index) m_slots.resize(static_cast<std::size_t>(ExtensionId::Count));

    auto entry = std::make_unique<Entry>();
    entry->id = id;
    entry->priority = priority;
    entry->loader = std::move(loader);
    entry->name = name.empty() ? std::string(toString(id)) : std::move(name);

    {
        // ワーカーが登録順を走査している最中でも壊れないよう排他を取る。
        std::lock_guard<std::mutex> lock(m_regMutex);
        if (m_slots[index]) return false;                          // 重複登録
        m_slots[index] = std::move(entry);
        m_regOrder.push_back(index);
    }

    if (m_started.load(std::memory_order_acquire)) {
        // start() 後の後付け登録 (v3): 待機中のワーカを起こしてロードさせる。
        requestWork();
    }
    return true;
}

bool ExtensionManager::start(ExtensionHost host) {
    bool expected = false;
    if (!m_started.compare_exchange_strong(expected, true,
                                           std::memory_order_acq_rel)) {
        log::info("ExtensionManager: already started");
        return false;
    }
    if (m_config.maxConcurrent == 0) m_config.maxConcurrent = 1;

    m_host = host;
    m_startTime = std::chrono::steady_clock::now();

    // ワーカは停止要求まで待機し続ける (v3: 起動後の有効化 / 後付け登録に対応するため)。
    m_worker = std::jthread([this](std::stop_token token) { run(std::move(token)); });

    if (m_regOrder.empty()) {
        m_totalElapsed = std::chrono::milliseconds{0};
        m_totalCaptured = true;
        m_settled.store(true, std::memory_order_release);
        log::info("Phase 2: no extension registered");
        return true;
    }

    log::info("Phase 2: background load start (budget %lld ms, %zu entries, concurrency %zu)",
              static_cast<long long>(m_config.totalBudget.count()),
              m_regOrder.size(), m_config.maxConcurrent);
    return true;
}

void ExtensionManager::requestStop() noexcept {
    if (m_worker.joinable()) m_worker.request_stop();
    notifyWorker(); // 待機中のワーカを起こして終了処理へ進める
}

void ExtensionManager::join() {
    requestStop(); // 未要求ならここで要求する (ワーカが待機無限に居るため必須)
    if (m_worker.joinable()) m_worker.join();
    drain(); // 残った完了通知を回収する
}

ExtensionManager::~ExtensionManager() {
    requestStop();
    join();
}

// ---------------------------------------------------------------------------
// メインスレッド API
// ---------------------------------------------------------------------------

void ExtensionManager::tick() {
    drain(); // 完了デキュー + UI バインド。内部で終了チェックする程度のコストのみ。
}

void ExtensionManager::setReadyHook(ReadyHook hook) {
    m_readyHook = std::move(hook);
}

// ---------------------------------------------------------------------------
// 拡張の ON/OFF (v3)
// ---------------------------------------------------------------------------

ExtensionManager::Entry* ExtensionManager::slot(ExtensionId id) noexcept {
    const auto index = static_cast<std::size_t>(id);
    if (index >= m_slots.size()) return nullptr;
    return m_slots[index].get();
}

const ExtensionManager::Entry* ExtensionManager::slot(ExtensionId id) const noexcept {
    const auto index = static_cast<std::size_t>(id);
    if (index >= m_slots.size()) return nullptr;
    return m_slots[index].get();
}

bool ExtensionManager::hasClaimableWork() const noexcept {
    std::lock_guard<std::mutex> lock(m_regMutex);
    for (const auto& entry : m_slots) {
        if (!entry) continue;
        if (!entry->enabled.load(std::memory_order_acquire)) continue;
        if (entry->state.load(std::memory_order_acquire) != ExtensionState::Pending) continue;
        if (entry->claimed.load(std::memory_order_acquire)) continue;
        return true;
    }
    return false;
}

void ExtensionManager::notifyWorker() noexcept {
    m_cv.notify_all();
}

void ExtensionManager::requestWork() noexcept {
    m_settled.store(false, std::memory_order_release);
    m_workRequested.store(true, std::memory_order_release);
    notifyWorker();
}

bool ExtensionManager::setEnabled(ExtensionId id, bool enable) noexcept {
    Entry* entry = slot(id);
    if (!entry) return false;

    const ExtensionState current = entry->state.load(std::memory_order_acquire);

    if (!enable) {
        entry->enabled.store(false, std::memory_order_release);

        if (current == ExtensionState::Pending || current == ExtensionState::Ready) {
            // 未着手 / バインド済み → 非活性化。
            // Ready の instance は破棄しない (再有効化で即座に ReadyHook へ復帰できる)。
            entry->state.store(ExtensionState::Cancelled, std::memory_order_release);
        }
        // Loading 中は完了を待つ (drain() が結果を破棄して Cancelled にする)
        log::info("Extension OFF: %.*s", static_cast<int>(entry->name.size()),
                  entry->name.c_str());
        return true;
    }

    entry->enabled.store(true, std::memory_order_release);

    if (current == ExtensionState::Ready) {
        return true; // 既に活性
    }

    if (current == ExtensionState::Cancelled && entry->instance) {
        // 無効化前にロード済み → 保存した instance を ReadyHook へ再配線する。
        if (m_readyHook) {
            try {
                m_readyHook(entry->id, m_host, entry->instance);
            } catch (...) {
                log::info("ReadyHook threw an exception (re-enable): %.*s",
                          static_cast<int>(entry->name.size()), entry->name.c_str());
            }
        }
        entry->state.store(ExtensionState::Ready, std::memory_order_release);
        log::info("Extension ON: %.*s (rebound)", static_cast<int>(entry->name.size()),
                  entry->name.c_str());
        return true;
    }

    if (current == ExtensionState::Pending) {
        // 未着手ならワーカに通知する (着手済みならそのまま完了を待つ)
        if (!entry->claimed.load(std::memory_order_acquire)) requestWork();
        log::info("Extension ON: %.*s (pending)", static_cast<int>(entry->name.size()),
                  entry->name.c_str());
        return true;
    }

    if (current == ExtensionState::Failed || current == ExtensionState::TimedOut ||
        current == ExtensionState::Cancelled) {
    // ロード失敗 / 予算超過 / 未ロード → 再度ロードを試みる
        entry->claimed.store(false, std::memory_order_release);
        entry->state.store(ExtensionState::Pending, std::memory_order_release);
        entry->message.clear();
        requestWork();
        log::info("Extension ON: %.*s (retry)", static_cast<int>(entry->name.size()),
                  entry->name.c_str());
        return true;
    }

    // Loading 中はそのまま完了を待つ
    log::info("Extension ON: %.*s", static_cast<int>(entry->name.size()), entry->name.c_str());
    return true;
}

bool ExtensionManager::enabled(ExtensionId id) const noexcept {
    const Entry* entry = slot(id);
    if (!entry) return false;
    return entry->enabled.load(std::memory_order_acquire);
}

// ---------------------------------------------------------------------------
// 状態問い合わせ (アトミック読みのみ)
// ---------------------------------------------------------------------------

ExtensionState ExtensionManager::state(ExtensionId id) const noexcept {
    const auto index = static_cast<std::size_t>(id);
    if (index >= m_slots.size() || !m_slots[index]) return ExtensionState::Pending;
    return m_slots[index]->state.load(std::memory_order_acquire);
}

bool ExtensionManager::isReady(ExtensionId id) const noexcept {
    return state(id) == ExtensionState::Ready;
}

bool ExtensionManager::registered(ExtensionId id) const noexcept {
    const auto index = static_cast<std::size_t>(id);
    return index < m_slots.size() && m_slots[index] != nullptr;
}

std::size_t ExtensionManager::registeredCount() const noexcept {
    std::size_t count = 0;
    for (const auto& slot : m_slots) {
        if (slot) ++count;
    }
    return count;
}

std::size_t ExtensionManager::readyCount() const noexcept {
    std::size_t count = 0;
    for (const auto& slot : m_slots) {
        if (slot && slot->state.load(std::memory_order_acquire) == ExtensionState::Ready) ++count;
    }
    return count;
}

bool ExtensionManager::allSettled() const noexcept {
    if (!m_started.load(std::memory_order_acquire)) return registeredCount() == 0;
    for (const auto& slot : m_slots) {
        if (!slot) continue;
        const auto value = slot->state.load(std::memory_order_acquire);
        if (value == ExtensionState::Loading) return false;   // 無効化済みでも完了待ち
        if (value == ExtensionState::Pending &&
            slot->enabled.load(std::memory_order_acquire))
            return false;
    }
    return true;
}

// ---------------------------------------------------------------------------
// 詳細情報 (メインスレッド専用)
// ---------------------------------------------------------------------------

std::string_view ExtensionManager::name(ExtensionId id) const noexcept {
    const auto index = static_cast<std::size_t>(id);
    if (index >= m_slots.size() || !m_slots[index]) return toString(id);
    return m_slots[index]->name;
}

std::string ExtensionManager::message(ExtensionId id) const {
    const auto index = static_cast<std::size_t>(id);
    if (index >= m_slots.size() || !m_slots[index]) return {};
    return m_slots[index]->message;
}

std::chrono::milliseconds ExtensionManager::elapsed(ExtensionId id) const noexcept {
    const auto index = static_cast<std::size_t>(id);
    if (index >= m_slots.size() || !m_slots[index]) return std::chrono::milliseconds{0};
    return m_slots[index]->elapsed;
}

std::chrono::milliseconds ExtensionManager::totalElapsed() const noexcept {
    return m_totalElapsed;
}

bool ExtensionManager::waitForSettled(std::chrono::milliseconds timeout) {
    using Clock = std::chrono::steady_clock;
    const auto deadline = Clock::now() + timeout;
    while (true) {
        drain();
        if (m_settled.load(std::memory_order_acquire) || allSettled()) return true;
        if (Clock::now() >= deadline) return false;
        std::unique_lock<std::mutex> lock(m_mutex);
        m_cv.wait_for(lock, std::chrono::milliseconds(2)); // 通知待ち (2ms 刻み)
    }
}

// ---------------------------------------------------------------------------
// バックグラウンド実行
// ---------------------------------------------------------------------------

std::vector<std::size_t> ExtensionManager::sortedOrder() const {
    std::vector<std::size_t> order;
    {
        std::lock_guard<std::mutex> lock(m_regMutex);
        order = m_regOrder; // 登録順を保持したまま安定ソート
    }
    std::stable_sort(order.begin(), order.end(), [this](std::size_t lhs, std::size_t rhs) {
        return m_slots[lhs]->priority < m_slots[rhs]->priority;
    });
    return order;
}

void ExtensionManager::enqueue(Completion completion) {
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        m_queue.push_back(std::move(completion));
    }
    m_cv.notify_all();
}

void ExtensionManager::executeOne(std::size_t index, std::stop_token token,
                                  std::chrono::steady_clock::time_point deadline) {
    using Clock = std::chrono::steady_clock;
    auto& entry = *m_slots[index];
    const auto startedAt = Clock::now();

    // 無効化された Entry はロードしない (v3)
    if (!entry.enabled.load(std::memory_order_acquire)) {
        enqueue({index, LoadResult::error("無効化によりキャンセル"), std::chrono::milliseconds{0},
                 ExtensionState::Cancelled});
        return;
    }

    entry.state.store(ExtensionState::Loading, std::memory_order_release);

    LoadResult result;
    if (token.stop_requested()) {
        result = LoadResult::error("停止要求によりキャンセル");
    } else {
        const LoadContext context{m_host, token, deadline};
        try {
            result = entry.loader(context);
        } catch (const std::exception& ex) {
            result = LoadResult::error(std::string("例外: ") + ex.what());
        } catch (...) {
            result = LoadResult::error("未知の例外");
        }
    }

    const auto elapsed =
        std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now() - startedAt);

    ExtensionState finalState = ExtensionState::Failed;
    if (result.ok) {
        finalState = ExtensionState::Ready;
    } else if (result.timedOut) {
        finalState = ExtensionState::TimedOut;
    } else if (token.stop_requested()) {
        finalState = ExtensionState::Cancelled;
    }

    log::info("Phase 2: %.*s -> %s (%lld ms)%s%s",
              static_cast<int>(entry.name.size()), entry.name.c_str(),
              std::string(toString(finalState)).c_str(),
              static_cast<long long>(elapsed.count()),
              result.ok ? "" : " : ",
              result.ok ? "" : result.message.c_str());

    enqueue({index, std::move(result), elapsed, finalState});
}

void ExtensionManager::run(std::stop_token token) {
    using Clock = std::chrono::steady_clock;
    Clock::time_point deadline = m_startTime + m_config.totalBudget;
    std::vector<std::future<void>> inflight;
    inflight.reserve(m_config.maxConcurrent);

    const std::string timeoutReason =
        "ロード予算 (" + std::to_string(m_config.totalBudget.count()) + " ms) を超過";
    const std::string cancelReason = "停止要求によりキャンセル";

    auto reapFinished = [&]() {
        for (auto it = inflight.begin(); it != inflight.end();) {
            if (it->wait_for(std::chrono::milliseconds(0)) == std::future_status::ready) {
                it->get(); // executeOne は例外を内部で捕捉している
                it = inflight.erase(it);
            } else {
                ++it;
            }
        }
    };

    // 未着手 (Pending かつ未着手済みフラグなし) の Entry を 1 件だけ受け取る。
    // 無効 (enabled=false) の Entry は絶対に拾わない。
    auto claimNext = [this]() -> int {
        const std::vector<std::size_t> order = sortedOrder();
        std::lock_guard<std::mutex> lock(m_regMutex);
        for (std::size_t index : order) {
            Entry& entry = *m_slots[index];
            if (!entry.enabled.load(std::memory_order_acquire)) continue;
            if (entry.state.load(std::memory_order_acquire) != ExtensionState::Pending) continue;
            bool expected = false;
            if (!entry.claimed.compare_exchange_strong(expected, true, std::memory_order_acq_rel))
                continue;
            return static_cast<int>(index);
        }
        return -1;
    };

    // 予算超過 / 停止時に、まだ着手していない Pending を一括で確定させる。
    auto sweepPending = [&](ExtensionState finalState, const std::string& message) {
        std::lock_guard<std::mutex> lock(m_regMutex);
        for (std::size_t index = 0; index < m_slots.size(); ++index) {
            Entry* entry = m_slots[index].get();
            if (!entry) continue;
            if (!entry->enabled.load(std::memory_order_acquire)) continue;
            if (entry->state.load(std::memory_order_acquire) != ExtensionState::Pending) continue;
            bool expected = false;
            if (!entry->claimed.compare_exchange_strong(expected, true, std::memory_order_acq_rel))
                continue;
            LoadResult result = finalState == ExtensionState::TimedOut
                                     ? LoadResult::timeout(message)
                                     : LoadResult::error(message);
            enqueue({index, std::move(result), std::chrono::milliseconds{0}, finalState});
        }
    };

    while (true) {
        if (token.stop_requested()) {
            sweepPending(ExtensionState::Cancelled, cancelReason);
            break;
        }

        reapFinished();

        // 予算内なら空き枠へ次のローダを投入 (std::async による並列ロード)
        bool spendBudget = m_config.failRemainingOnTimeout && Clock::now() >= deadline;
        while (inflight.size() < m_config.maxConcurrent) {
            if (spendBudget) {
                sweepPending(ExtensionState::TimedOut, timeoutReason);
                break;
            }
            const int claimed = claimNext();
            if (claimed < 0) break;
            const std::size_t index = static_cast<std::size_t>(claimed);
            inflight.push_back(std::async(std::launch::async, [this, index, token, deadline] {
                executeOne(index, token, deadline);
            }));
        }

        if (!inflight.empty()) {
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
            continue;
        }

        // 手持ちのロードが尽きた。着手可能な Entry が残っていればトップへ戻る
        // (予算超過時はこのパスで sweep され、次の回で空になる → 無限ループしない)。
        if (hasClaimableWork()) continue;

        // 待機: 新規の仕事 (後付け登録 / 有効化) または停止要求
        {
            std::unique_lock<std::mutex> lock(m_mutex);
            m_cv.wait_for(lock, std::chrono::milliseconds(100), [&] {
                return token.stop_requested() || m_workRequested.load(std::memory_order_acquire);
            });
        }
        m_workRequested.store(false, std::memory_order_release);

        if (token.stop_requested()) {
            sweepPending(ExtensionState::Cancelled, cancelReason);
            break;
        }

        // 新しいバッチ (遅延有効化 / 後付け登録) は予算を立て直す
        if (hasClaimableWork()) deadline = Clock::now() + m_config.totalBudget;
    }

    for (auto& future : inflight) {
        if (future.valid()) future.get();
    }
}

// ---------------------------------------------------------------------------
// 完了デキュー (UI バインド)
// ---------------------------------------------------------------------------

void ExtensionManager::drain() {
    std::vector<Completion> batch;
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        batch.swap(m_queue);
    }

    for (auto& completion : batch) {
        if (completion.index >= m_slots.size()) continue;
        auto& entry = *m_slots[completion.index];

        const ExtensionState finalState = completion.finalState;
        entry.message = std::move(completion.result.message);
        entry.elapsed = completion.elapsed;

        // ロード中に無効化された場合は結果を破棄する (v3)
        if (!entry.enabled.load(std::memory_order_acquire)) {
            entry.instance.reset();
            entry.state.store(ExtensionState::Cancelled, std::memory_order_release);
            continue;
        }

        entry.instance = std::move(completion.result.instance);

        // ReadyHook で UI / 制御系への配線を済ませてから状態を公開する。
        // (UI が Ready を見た時点で必ずバインド済み、という不変条件を守る)
        if (finalState == ExtensionState::Ready && m_readyHook) {
            try {
                m_readyHook(entry.id, m_host, entry.instance);
            } catch (...) {
                log::info("ReadyHook threw an exception: %.*s",
                          static_cast<int>(entry.name.size()), entry.name.c_str());
            }
        }
        entry.state.store(finalState, std::memory_order_release);
    }

    if (!m_settled.load(std::memory_order_acquire) && allSettled()) {
        if (!m_totalCaptured) {
            m_totalElapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::steady_clock::now() - m_startTime);
            m_totalCaptured = true;
        }
        m_settled.store(true, std::memory_order_release);
        m_cv.notify_all();
    }
}

} // namespace nanoalpha
