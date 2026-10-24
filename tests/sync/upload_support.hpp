#pragma once

#include "support.hpp"

namespace onedrive::test::sync::upload_cases {

class TransferObserver final : public onedrive::events::Observer {
public:
    mutable std::vector<onedrive::events::UploadProgressEvent> progress;
    mutable std::optional<onedrive::events::SyncSummaryEvent> summary;

private:
    void on_event(const onedrive::events::Event& event) const override {
        if (const auto* value =
                std::get_if<onedrive::events::UploadProgressEvent>(&event)) {
            progress.push_back(*value);
        } else if (const auto* value =
                       std::get_if<onedrive::events::SyncSummaryEvent>(
                           &event
                       )) {
            summary = *value;
        }
    }
};

} // namespace onedrive::test::sync::upload_cases
