#include "support.hpp"
#include "onedrive/exit_code.hpp"
#include "onedrive/sync/blocked_policy.hpp"

#include <algorithm>
#include "upload_cases.hpp"

namespace onedrive::test::sync::upload_cases {

namespace exit_code = onedrive::exit_code;
using namespace onedrive::test::sync;

int test_missing_upload_source() {
    TemporaryDirectory temporary;
    for (const bool interrupted : {false, true}) {
        for (const bool moved : {false, true}) {
            for (const auto policy : {
                     onedrive::sync::DeletePolicy::propagate,
                     onedrive::sync::DeletePolicy::preserve,
                 }) {
                const auto root =
                    temporary.path() /
                    (std::to_string(interrupted) + std::to_string(moved) +
                     std::to_string(
                         policy == onedrive::sync::DeletePolicy::preserve
                     ));
                std::filesystem::create_directories(root);
                const auto local = root / "file.txt";
                const auto renamed = root / "renamed.txt";
                std::ofstream{local} << "data";
                FakeItemStore items;
                items.saved_delta_link = "saved";
                items.items.emplace(
                    "existing", tracked_item(root, "existing", "file.txt")
                );
                std::ofstream{local} << "payload";
                FakeGraphClient graph;
                FakeMetrics metrics;
                auto config = config_for(root, false);
                config.sync_mode = onedrive::sync::SyncMode::bidirectional;
                config.delete_policy = policy;
                config.upload_concurrency = 1;
                std::ostringstream output;
                std::ostringstream errors;
                const onedrive::cli::Console console{
                    {.color = onedrive::cli::ColorMode::never,
                     .output = onedrive::cli::OutputMode::json},
                    output,
                    errors,
                };
                const auto change_source = [&] {
                    if (moved) {
                        std::filesystem::rename(local, renamed);
                    } else {
                        std::filesystem::remove(local);
                    }
                };
                const auto synchronize = [&] {
                    return onedrive::sync::SyncEngine{
                        config, graph, items, metrics, &console
                    }
                        .synchronize();
                };
                if (interrupted) {
                    items.fail_commit_upload = true;
                    try {
                        static_cast<void>(synchronize());
                        return fail(
                            "upload commit interruption was not reported"
                        );
                    } catch (const std::runtime_error& error) {
                        if (!std::string_view{error.what()}.contains(
                                "upload commit failure"
                            )) {
                            return fail(
                                "upload interruption reported the wrong error"
                            );
                        }
                    }
                    items.fail_commit_upload = false;
                    change_source();
                } else {
                    graph.before_upload_return = change_source;
                    if (synchronize() != exit_code::success) {
                        return fail(
                            "source removal during upload blocked its commit"
                        );
                    }
                }
                auto echo = file("existing", "file.txt", 7);
                echo.etag = interrupted ? "uploaded-etag-2" : "uploaded-etag-1";
                graph.changes = {echo};
                graph.contents["existing"] = "payload";
                if (synchronize() != exit_code::success ||
                    !items.pending_uploads("me").empty() ||
                    graph.delta_requests.size() != 2 ||
                    graph.download_count != 0 ||
                    std::filesystem::exists(local) ||
                    std::ranges::any_of(
                        std::filesystem::directory_iterator{root},
                        [](const auto& entry) {
                            return entry.path().filename().string().contains(
                                ".onedrive-upload-"
                            );
                        }
                    )) {
                    return fail(
                        "missing source retained its journal or blocked delta "
                        "queries"
                    );
                }
                const bool preserves =
                    policy == onedrive::sync::DeletePolicy::preserve;
                const auto retained = items.find("me", "existing");
                if (graph.deleted_items.size() != (preserves ? 0U : 1U) ||
                    retained.has_value() != preserves ||
                    (retained && (retained->local_device != 0 ||
                                  retained->local_inode != 0)) ||
                    (moved && (!std::filesystem::exists(renamed) ||
                               graph.uploaded_paths.back() != "renamed.txt")) ||
                    !(output.str() + errors.str())
                         .contains(
                             interrupted ? "pending_upload_source_missing"
                                         : "local_upload_source_missing"
                         )) {
                    return fail(
                        "missing upload source did not honor deletion policy "
                        "or preserve moved content"
                    );
                }
                graph.changes.clear();
                if (synchronize() != exit_code::success ||
                    graph.delta_requests.size() != 3) {
                    return fail(
                        "missing upload source blocked a later synchronization"
                    );
                }
            }
        }
    }
    for (const bool symlink : {false, true}) {
        const auto root =
            temporary.path() / (symlink ? "symlink" : "directory");
        std::filesystem::create_directories(root);
        const auto local = root / "file.txt";
        std::ofstream{local} << "payload";
        FakeItemStore items;
        items.saved_delta_link = "saved";
        items.fail_commit_upload = true;
        FakeGraphClient graph;
        FakeMetrics metrics;
        const onedrive::cli::Console console;
        auto config = config_for(root, false);
        config.sync_mode = onedrive::sync::SyncMode::bidirectional;
        const auto synchronize = [&] {
            return onedrive::sync::SyncEngine{
                config, graph, items, metrics, &console
            }
                .synchronize();
        };
        try {
            static_cast<void>(synchronize());
            return fail(
                "unsafe-source setup did not preserve an upload journal"
            );
        } catch (const std::runtime_error&) {
        }
        items.fail_commit_upload = false;
        std::filesystem::remove(local);
        if (symlink) {
            std::filesystem::create_symlink(root / "absent", local);
        } else {
            std::filesystem::create_directory(local);
        }
        try {
            static_cast<void>(synchronize());
            return fail(
                "unsafe replacement source was accepted during recovery"
            );
        } catch (const std::runtime_error& error) {
            if (!std::string_view{error.what()}.contains("unexpected type")) {
                return fail(
                    "unsafe replacement source reported the wrong error"
                );
            }
        }
        if (items.pending_uploads("me").size() != 1 ||
            graph.delta_requests.size() != 1) {
            return fail(
                "unsafe replacement source discarded recovery evidence"
            );
        }
    }
    return EXIT_SUCCESS;
}

} // namespace onedrive::test::sync::upload_cases
