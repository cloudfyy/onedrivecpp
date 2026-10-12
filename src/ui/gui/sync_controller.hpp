#pragma once

#include "onedrive/config/config.hpp"
#include "onedrive/ui/common/observer.hpp"
#include "onedrive/sync/core/engine.hpp"

#include <QObject>
#include <QString>

#include <deque>
#include <functional>
#include <mutex>
#include <thread>
#include <vector>

namespace onedrive::gui {

using SynchronizationFunction = std::function<
    int(config::Config, const events::Observer&, std::stop_token)>;
using CachedSynchronizationFunction = std::function<
    int(config::Config,
        const events::Observer&,
        std::stop_token,
        sync::PreviewCache*)>;

class SyncController final : public QObject {
    Q_OBJECT

public:
    explicit SyncController(
        SynchronizationFunction synchronize = {}, QObject* parent = nullptr
    );
    explicit SyncController(
        CachedSynchronizationFunction synchronize, QObject* parent = nullptr
    );
    ~SyncController() override;
    void start(const std::filesystem::path& config_file, bool preview);
    void cancel();
    void invalidate_preview();
    [[nodiscard]] bool running() const noexcept;

Q_SIGNALS:
    void started(bool preview);
    void
    events_ready(const std::vector<events::Event>& batch, std::size_t omitted);
    void finished(int result, QString error, bool preview);

private:
    void queue_event(const events::Event& event);
    void drain_events();

    CachedSynchronizationFunction synchronize_;
    sync::PreviewCache preview_cache_;
    std::filesystem::path preview_config_file_;
    std::string preview_config_fingerprint_;
    std::jthread worker_;
    bool running_{};
    std::mutex events_mutex_;
    std::deque<events::Event> pending_events_;
    std::size_t omitted_events_{};
    bool delivery_pending_{};
};

} // namespace onedrive::gui
