#include "onedrive/events/observer.hpp"
#include "sync/support.hpp"

#include <array>
#include <thread>
#include <variant>

namespace {

using namespace onedrive::events;
using onedrive::test::fail;
using onedrive::util::ProgressState;

class CapturingObserver final : public Observer {
public:
    mutable std::vector<Event> events;
    bool reject{false};

private:
    void on_event(const Event& event) const override {
        if (reject) {
            throw std::runtime_error{"observer rejected event"};
        }
        events.push_back(event);
    }
};

int test_payloads() {
    CapturingObserver observer;
    std::string text{"Working"};
    std::vector<Field> fields{
        {.label = "Files:", .key = "files", .value = "12"},
    };
    observer.message(MessageKind::information, "phase", text);
    observer.section("summary", "Summary", fields);
    observer.delta_progress(2, 350, ProgressState::ongoing);
    observer.delta_summary({
        .pages = 3,
        .scanned_items = 412,
        .unique_changes = 400,
        .files = 300,
        .directories = 90,
        .deletions = 10,
    });
    observer.blocked_item("file.txt", "local_modification", "modified");
    observer.download_progress(
        1,
        2,
        5,
        10,
        ProgressState::ongoing,
        {
            .bytes_per_second = 2'048,
            .estimated_seconds_remaining = 65,
            .elapsed_milliseconds = 500,
        }
    );
    observer.end_download_progress();
    text = "Changed";
    fields.front().value = "0";

    if (observer.events.size() != 7) {
        return fail("observer did not publish every event");
    }
    const auto& message = std::get<MessageEvent>(observer.events[0]);
    const auto& section = std::get<SectionEvent>(observer.events[1]);
    const auto& delta = std::get<DeltaProgressEvent>(observer.events[2]);
    const auto& summary =
        std::get<DeltaSummaryEvent>(observer.events[3]).summary;
    const auto& blocked = std::get<BlockedItemEvent>(observer.events[4]);
    const auto& download = std::get<DownloadProgressEvent>(observer.events[5]);
    if (message.kind != MessageKind::information || message.event != "phase" ||
        message.text != "Working" || section.event != "summary" ||
        section.title != "Summary" || section.fields.size() != 1 ||
        section.fields[0].label != "Files:" ||
        section.fields[0].key != "files" || section.fields[0].value != "12" ||
        delta.pages != 2 || delta.items != 350 ||
        delta.state != ProgressState::ongoing || summary.pages != 3 ||
        summary.scanned_items != 412 || summary.unique_changes != 400 ||
        summary.files != 300 || summary.directories != 90 ||
        summary.deletions != 10 || blocked.path != "file.txt" ||
        blocked.reason_code != "local_modification" ||
        blocked.reason_message != "modified" || download.completed_files != 1 ||
        download.file_count != 2 || download.downloaded != 5 ||
        download.total != 10 || download.state != ProgressState::ongoing ||
        download.metrics.bytes_per_second != 2'048 ||
        download.metrics.estimated_seconds_remaining != 65 ||
        download.metrics.elapsed_milliseconds != 500 ||
        !std::holds_alternative<EndDownloadProgressEvent>(observer.events[6])) {
        return fail("observer changed or borrowed event payloads");
    }
    if (download_progress_percentage(1, 2, 10, 10, ProgressState::ongoing) !=
            99 ||
        download_progress_percentage(2, 2, 10, 10, ProgressState::completed) !=
            100 ||
        download_progress_percentage(0, 0, 0, 0, ProgressState::ongoing) != 0 ||
        download_progress_percentage(0, 0, 0, 0, ProgressState::completed) !=
            100) {
        return fail("neutral progress percentage changed completion semantics");
    }
    return EXIT_SUCCESS;
}

int test_workers_and_errors() {
    CapturingObserver observer;
    {
        std::array<std::jthread, 4> workers;
        for (auto& worker : workers) {
            worker = std::jthread{[&observer] {
                for (int index = 0; index < 100; ++index) {
                    observer.message(
                        MessageKind::information,
                        "worker",
                        std::to_string(index)
                    );
                }
            }};
        }
    }
    if (observer.events.size() != 400) {
        return fail("worker events were not serialized without loss");
    }
    std::array<int, 100> counts{};
    for (const auto& event : observer.events) {
        const auto& message = std::get<MessageEvent>(event);
        ++counts.at(static_cast<std::size_t>(std::stoi(message.text)));
    }
    if (std::ranges::any_of(counts, [](int count) { return count != 4; })) {
        return fail("worker event payloads were lost or duplicated");
    }
    observer.reject = true;
    if (!onedrive::test::throws_with<std::runtime_error>(
            [&observer] { observer.end_download_progress(); },
            "observer rejected event"
        )) {
        return fail("observer exceptions did not reach the caller");
    }
    observer.reject = false;
    observer.end_download_progress();
    if (observer.events.size() != 401) {
        return fail("observer did not recover after a callback exception");
    }
    return EXIT_SUCCESS;
}

int test_headless_sync() {
    using namespace onedrive::test::sync;
    TemporaryDirectory temporary;
    const auto root = temporary.path() / "sync";
    FakeGraphClient graph;
    graph.changes = {file("file", "file.txt", 4)};
    graph.contents["file"] = "data";
    FakeItemStore items;
    FakeMetrics metrics;
    CapturingObserver observer;
    auto config = config_for(root, false);
    if (onedrive::sync::SyncEngine{config, graph, items, metrics, &observer}
                .synchronize() != 0 ||
        onedrive::test::read_file(root / "file.txt") != "data" ||
        !metrics.last_success) {
        return fail("sync did not work with a non-console observer");
    }
    const bool completed =
        std::ranges::any_of(observer.events, [](const Event& event) {
            const auto* progress = std::get_if<DownloadProgressEvent>(&event);
            return progress != nullptr &&
                   progress->state == ProgressState::completed &&
                   progress->completed_files == 1 &&
                   progress->file_count == 1 && progress->downloaded == 4 &&
                   progress->total == 4;
        });
    if (!completed) {
        return fail(
            "headless observer did not receive completed sync progress"
        );
    }
    return EXIT_SUCCESS;
}

} // namespace

int main() {
    if (test_payloads() != EXIT_SUCCESS ||
        test_workers_and_errors() != EXIT_SUCCESS ||
        test_headless_sync() != EXIT_SUCCESS) {
        return EXIT_FAILURE;
    }
    return EXIT_SUCCESS;
}
