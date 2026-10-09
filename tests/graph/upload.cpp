#include "support.hpp"

namespace {

using namespace onedrive::test::graph;

int test_simple_file_uploads() {
    const auto uploaded_json =
        R"json({"id":"file-id","name":"new #1.txt","eTag":"new-etag","size":7,"fileSystemInfo":{"lastModifiedDateTime":"2026-10-04T09:00:00Z"},"parentReference":{"id":"folder-id","path":"/drive/root:/Folder A"},"file":{}})json";
    auto transport = std::make_unique<
        FakeTransport>(std::deque<onedrive::http::HttpResult>{
        token_response("access-secret", "existing-refresh"),
        json_response(201, uploaded_json),
        json_response(200, uploaded_json),
        graph_error_response(409, "", "name already exists"),
        graph_error_response(
            403, "quotaLimitReached", "OneDrive quota exceeded"
        ),
    });
    auto* transport_pointer = transport.get();
    onedrive::graph::MicrosoftGraphClient client{
        wrap_transport(std::move(transport)),
        wrap_token_store(
            std::make_unique<FakeTokenStore>(std::string{"existing-refresh"})
        ),
        auth_options(),
        {
            .drive_id = "me",
            .endpoint = "https://graph.example.test/v1.0",
        },
    };
    const auto source = test_directory() / "upload-source.txt";
    {
        std::ofstream output{source, std::ios::binary};
        output << "payload";
    }
    const auto created =
        client.upload_file("Folder A/new #1.txt", std::nullopt, "", source);
    const auto updated = client.upload_file(
        "Folder A/new #1.txt", std::string{"file/id"}, "old-etag", source
    );
    try {
        static_cast<void>(
            client.upload_file("Folder A/new #1.txt", std::nullopt, "", source)
        );
        return fail("Graph upload conflict was accepted");
    } catch (const onedrive::graph::UploadConflictError&) {
    }
    try {
        static_cast<void>(
            client.upload_file("Folder A/quota.txt", std::nullopt, "", source)
        );
        return fail("Graph upload quota failure was accepted");
    } catch (const onedrive::graph::UploadResourceError& error) {
        if (error.reason_code() != "remote_quota" ||
            std::string_view{error.what()} != "OneDrive quota exceeded") {
            return fail("Graph upload quota failure lost its reason");
        }
    }
    if (created.id != "file-id" || updated.etag != "new-etag" ||
        transport_pointer->queued.requests.size() != 5 ||
        transport_pointer->queued.requests[1].method !=
            onedrive::http::HttpMethod::put ||
        transport_pointer->queued.requests[1].url !=
            "https://graph.example.test/v1.0/me/drive/root:/Folder%20A/"
            "new%20%231.txt:/content?"
            "@microsoft.graph.conflictBehavior=fail" ||
        transport_pointer->queued.requests[1].body != "payload" ||
        transport_pointer->queued.requests[2].url !=
            "https://graph.example.test/v1.0/me/drive/items/file%2Fid/"
            "content" ||
        !has_header(
            transport_pointer->queued.requests[2], "If-Match: old-etag"
        )) {
        return fail("Graph simple uploads were not conditional or encoded");
    }
    return EXIT_SUCCESS;
}

int test_simple_upload_cancellation_is_not_a_resource_failure() {
    auto transport = std::make_unique<FakeTransport>(
        std::deque<onedrive::http::HttpResult>{
            token_response(),
            std::unexpected(onedrive::http::HttpError{
                .code = onedrive::http::HttpErrorCode::cancelled,
                .message = "upload transport cancelled",
            }),
        }
    );
    auto* transport_pointer = transport.get();
    std::stop_source stop;
    transport_pointer->on_perform =
        [&](const onedrive::http::HttpRequest& request) {
            if (request.method == onedrive::http::HttpMethod::put) {
                stop.request_stop();
            }
        };
    onedrive::graph::MicrosoftGraphClient client{
        wrap_transport(std::move(transport)),
        wrap_token_store(std::make_unique<FakeTokenStore>(
            std::string{"existing-refresh"}
        )),
        auth_options(),
        {
            .drive_id = "me",
            .endpoint = "https://graph.example.test/v1.0",
        },
    };
    const auto source = test_directory() / "cancelled-upload-source.txt";
    {
        std::ofstream output{source, std::ios::binary};
        output << "payload";
    }

    try {
        static_cast<void>(client.upload_file(
            "cancelled.txt",
            std::nullopt,
            "",
            source,
            std::nullopt,
            {},
            stop.get_token()
        ));
        return fail("cancelled Graph upload was accepted");
    } catch (const onedrive::graph::RequestCancelledError&) {
    } catch (const onedrive::graph::UploadResourceError&) {
        return fail("Graph upload cancellation was reported as a resource error");
    }

    if (transport_pointer->queued.requests.size() != 2 ||
        transport_pointer->queued.requests.back().stop_token !=
            stop.get_token()) {
        return fail("Graph upload cancellation retried or lost its stop token");
    }
    return EXIT_SUCCESS;
}

int test_upload_session_cancellation_stages() {
    const auto run_cancelled_upload = [](
                                          std::deque<onedrive::http::HttpResult>
                                              responses,
                                          std::size_t cancel_on_request,
                                          std::size_t expected_requests,
                                          std::optional<
                                              onedrive::graph::UploadSession
                                          > session
                                      ) {
        auto transport = std::make_unique<FakeTransport>(
            std::move(responses)
        );
        auto* transport_pointer = transport.get();
        std::stop_source stop;
        std::size_t calls = 0;
        transport_pointer->on_perform =
            [&](const onedrive::http::HttpRequest&) {
                if (++calls == cancel_on_request) {
                    stop.request_stop();
                }
            };
        onedrive::graph::GraphOptions graph_options{
            .drive_id = "me",
            .endpoint = "https://graph.example.test/v1.0",
            .simple_upload_threshold_bytes = 1,
        };
        onedrive::graph::MicrosoftGraphClient client{
            wrap_transport(std::move(transport)),
            wrap_token_store(std::make_unique<FakeTokenStore>(
                std::string{"existing-refresh"}
            )),
            auth_options(),
            graph_options,
        };
        const auto source =
            test_directory() / "cancelled-session-upload-source.bin";
        {
            std::ofstream output{source, std::ios::binary};
            output << "xx";
        }
        std::size_t checkpoints = 0;
        try {
            static_cast<void>(client.upload_file(
                "cancelled-session.bin",
                std::nullopt,
                "",
                source,
                std::move(session),
                [&](const onedrive::graph::UploadSession&) {
                    ++checkpoints;
                },
                stop.get_token()
            ));
            return false;
        } catch (const onedrive::graph::RequestCancelledError&) {
            if (transport_pointer->queued.requests.size() !=
                    expected_requests ||
                transport_pointer->queued.requests.back().stop_token !=
                    stop.get_token() ||
                checkpoints != (cancel_on_request == 3 ? 1U : 0U)) {
                return false;
            }
            return true;
        } catch (const onedrive::graph::UploadResourceError&) {
            return false;
        } catch (const std::exception&) {
            return false;
        }
    };
    const auto cancelled = [] {
        return std::unexpected(onedrive::http::HttpError{
            .code = onedrive::http::HttpErrorCode::cancelled,
            .message = "upload transport cancelled",
        });
    };
    constexpr auto existing_token =
        R"({"token_type":"Bearer","expires_in":3600,"access_token":"access-secret","refresh_token":"existing-refresh"})";
    constexpr auto created_session =
        R"({"uploadUrl":"https://upload.example.test/session","expirationDateTime":"2099-10-09T11:00:00Z","nextExpectedRanges":["0-"]})";

    if (!run_cancelled_upload(
            {token_response(), cancelled()}, 2, 2, std::nullopt)) {
        return fail("simple Graph upload did not map cancellation correctly");
    }
    if (!run_cancelled_upload(
            {
                json_response(200, existing_token),
                cancelled(),
            },
            2,
            2,
            std::nullopt
        )) {
        return fail("upload-session creation did not map cancellation");
    }
    if (!run_cancelled_upload(
            {cancelled()},
            1,
            1,
            onedrive::graph::UploadSession{
                .upload_url = "https://upload.example.test/session",
                .expiration = "2099-10-09T11:00:00Z",
                .completed_bytes = 0,
            }
        )) {
        return fail("upload-session status did not map cancellation");
    }
    if (!run_cancelled_upload(
            {
                json_response(200, existing_token),
                json_response(200, created_session),
                cancelled(),
            },
            3,
            3,
            std::nullopt
        )) {
        return fail("upload-session fragment did not map cancellation");
    }
    return EXIT_SUCCESS;
}

int test_upload_sessions() {
    constexpr std::size_t chunk_size = 320U * 1024U;
    constexpr std::size_t total_size = chunk_size + 7U;
    const auto uploaded_json = std::format(
        R"json({{"id":"session-file","name":"large.bin","eTag":"new-etag","size":{},"fileSystemInfo":{{"lastModifiedDateTime":"2026-10-04T09:00:00Z"}},"parentReference":{{"id":"folder-id","path":"/drive/root:/Folder"}},"file":{{}}}})json",
        total_size
    );
    auto transport = std::make_unique<
        FakeTransport>(std::deque<onedrive::http::HttpResult>{
        onedrive::http::HttpResponse{
            .status_code = 200,
            .body = R"({"token_type":"Bearer","expires_in":3600,)"
                    R"("access_token":"access-secret",)"
                    R"("refresh_token":"existing-refresh"})",
        },
        onedrive::http::HttpResponse{
            .status_code = 200,
            .body =
                R"json({"uploadUrl":"https://upload.example.test/session?token=secret","expirationDateTime":"2026-10-05T09:00:00Z","nextExpectedRanges":["0-"]})json",
        },
        onedrive::http::HttpResponse{
            .status_code = 429,
            .headers = {{"Retry-After", "0"}},
            .body = R"json({"error":{"message":"throttled"}})json",
        },
        onedrive::http::HttpResponse{
            .status_code = 202,
            .body =
                R"json({"expirationDateTime":"2026-10-05T09:00:00Z","nextExpectedRanges":["327680-"]})json",
        },
        onedrive::http::HttpResponse{
            .status_code = 201,
            .body = uploaded_json,
        },
    });
    auto* transport_pointer = transport.get();
    std::vector<std::chrono::seconds> sleeps;
    onedrive::graph::MicrosoftGraphClient client{
        wrap_transport(std::move(transport)),
        wrap_token_store(
            std::make_unique<FakeTokenStore>(std::string{"existing-refresh"})
        ),
        auth_options(),
        {
            .drive_id = "me",
            .endpoint = "https://graph.example.test/v1.0",
            .simple_upload_threshold_bytes = 1,
            .upload_chunk_size_bytes = chunk_size,
            .upload_transport =
                {
                    .maximum_send_speed_bytes_per_second = 900,
                    .maximum_total_send_speed_bytes_per_second = 700,
                },
        },
        [&](std::chrono::seconds duration) { sleeps.push_back(duration); },
    };
    const auto source = test_directory() / "upload-session-source.bin";
    {
        std::ofstream output{source, std::ios::binary};
        output << std::string(total_size, 'x');
    }
    std::vector<onedrive::graph::UploadSession> upload_checkpoints;
    const auto uploaded = client.upload_file(
        "Folder/large.bin",
        std::nullopt,
        "",
        source,
        std::nullopt,
        [&](const onedrive::graph::UploadSession& checkpoint) {
            upload_checkpoints.push_back(checkpoint);
        }
    );
    const auto& requests = transport_pointer->queued.requests;
    if (uploaded.id != "session-file" || requests.size() != 5 ||
        requests[1].method != onedrive::http::HttpMethod::post ||
        requests[1].url !=
            "https://graph.example.test/v1.0/me/drive/root:/Folder/"
            "large.bin:/createUploadSession" ||
        !has_header(requests[1], "Authorization: ******") ||
        !requests[1].body.contains(
            R"("@microsoft.graph.conflictBehavior":"fail")"
        ) ||
        requests[2].url != "https://upload.example.test/session?token=secret" ||
        requests[2].body.size() != chunk_size ||
        requests[3].body != requests[2].body || requests[4].body.size() != 7 ||
        has_header(requests[2], "Authorization: ******") ||
        has_header(requests[3], "Authorization: ******") ||
        has_header(requests[4], "Authorization: ******") ||
        !has_header(requests[2], "Content-Range: bytes 0-327679/327687") ||
        !has_header(requests[4], "Content-Range: bytes 327680-327686/327687") ||
        requests[2].maximum_send_speed_bytes_per_second != 900 ||
        requests[4].maximum_send_speed_bytes_per_second != 900 ||
        !requests[2].upload_throttle || !requests[4].upload_throttle ||
        sleeps != std::vector{std::chrono::seconds{0}} ||
        upload_checkpoints.size() != 2 ||
        upload_checkpoints[0].completed_bytes != 0 ||
        upload_checkpoints[1].completed_bytes != chunk_size ||
        upload_checkpoints[1].expiration != "2026-10-05T09:00:00Z") {
        return fail("Graph upload session did not send contiguous fragments");
    }

    const auto update_source =
        test_directory() / "upload-session-update-source.bin";
    {
        std::ofstream output{update_source, std::ios::binary};
        output << "payload";
    }
    const auto update_json =
        R"json({"id":"file/id","name":"large.bin","eTag":"updated-etag","size":7,"fileSystemInfo":{"lastModifiedDateTime":"2026-10-04T09:00:00Z"},"parentReference":{"id":"folder-id","path":"/drive/root:/Folder"},"file":{}})json";
    auto update_transport = std::make_unique<
        FakeTransport>(std::deque<onedrive::http::HttpResult>{
        onedrive::http::HttpResponse{
            .status_code = 200,
            .body = R"({"token_type":"Bearer","expires_in":3600,)"
                    R"("access_token":"access-secret",)"
                    R"("refresh_token":"existing-refresh"})",
        },
        onedrive::http::HttpResponse{
            .status_code = 200,
            .body =
                R"json({"uploadUrl":"https://upload.example.test/update","expirationDateTime":"2026-10-05T09:00:00Z","nextExpectedRanges":["0-"]})json",
        },
        onedrive::http::HttpResponse{
            .status_code = 200,
            .body = update_json,
        },
    });
    auto* update_transport_pointer = update_transport.get();
    onedrive::graph::MicrosoftGraphClient update_client{
        wrap_transport(std::move(update_transport)),
        wrap_token_store(
            std::make_unique<FakeTokenStore>(std::string{"existing-refresh"})
        ),
        auth_options(),
        {
            .drive_id = "drive/id",
            .endpoint = "https://graph.example.test/v1.0",
            .simple_upload_threshold_bytes = 1,
            .upload_chunk_size_bytes = chunk_size,
            .upload_transport = {
                .maximum_send_speed_bytes_per_second = 900,
            },
        },
    };
    const auto updated = update_client.upload_file(
        "Folder/large.bin", std::string{"file/id"}, "old-etag", update_source
    );
    if (updated.etag != "updated-etag" ||
        update_transport_pointer->queued.requests.size() != 3 ||
        update_transport_pointer->queued.requests[1].url !=
            "https://graph.example.test/v1.0/drives/drive%2Fid/items/"
            "file%2Fid/createUploadSession" ||
        !has_header(
            update_transport_pointer->queued.requests[1], "If-Match: old-etag"
        ) ||
        !update_transport_pointer->queued.requests[1].body.contains(
            R"("@microsoft.graph.conflictBehavior":"replace")"
        ) ||
        update_transport_pointer->queued.requests[2]
                .maximum_send_speed_bytes_per_second != 900 ||
        has_header(
            update_transport_pointer->queued.requests[2],
            "Authorization: ******"
        )) {
        return fail("Graph update upload session was not conditional");
    }
    return EXIT_SUCCESS;
}
int test_upload_session_resume() {
    constexpr std::size_t chunk_size = 320U * 1024U;
    constexpr std::size_t total_size = chunk_size + 7U;
    const auto source = test_directory() / "upload-session-resume.bin";
    {
        std::ofstream output{source, std::ios::binary};
        output << std::string(total_size, 'x');
    }
    const auto uploaded_json =
        R"json({"id":"resumed-file","name":"large.bin","eTag":"resumed-etag","size":327687,"fileSystemInfo":{"lastModifiedDateTime":"2026-10-04T09:00:00Z"},"parentReference":{"id":"folder-id","path":"/drive/root:/Folder"},"file":{}})json";
    auto transport = std::make_unique<
        FakeTransport>(std::deque<onedrive::http::HttpResult>{
        onedrive::http::HttpResponse{
            .status_code = 200,
            .body =
                R"json({"expirationDateTime":"2099-10-05T10:00:00Z","nextExpectedRanges":["327680-"]})json",
        },
        onedrive::http::HttpResponse{
            .status_code = 201,
            .body = uploaded_json,
        },
    });
    auto* transport_pointer = transport.get();
    onedrive::graph::MicrosoftGraphClient client{
        wrap_transport(std::move(transport)),
        wrap_token_store(std::make_unique<FakeTokenStore>(std::nullopt)),
        auth_options(),
        {
            .drive_id = "me",
            .endpoint = "https://graph.example.test/v1.0",
            .simple_upload_threshold_bytes = total_size + 1U,
            .upload_chunk_size_bytes = chunk_size,
        },
    };
    std::vector<onedrive::graph::UploadSession> checkpoints;
    const auto uploaded = client.upload_file(
        "Folder/large.bin",
        std::nullopt,
        "",
        source,
        onedrive::graph::UploadSession{
            .upload_url = "https://upload.example.test/session?token=secret",
            .expiration = "2099-10-05T09:00:00Z",
            .completed_bytes = chunk_size,
        },
        [&](const onedrive::graph::UploadSession& checkpoint) {
            checkpoints.push_back(checkpoint);
        }
    );
    if (uploaded.id != "resumed-file" ||
        transport_pointer->queued.requests.size() != 2 ||
        transport_pointer->queued.requests[0].method !=
            onedrive::http::HttpMethod::get ||
        transport_pointer->queued.requests[1].method !=
            onedrive::http::HttpMethod::put ||
        has_header(
            transport_pointer->queued.requests[0], "Authorization: ******"
        ) ||
        has_header(
            transport_pointer->queued.requests[1], "Authorization: ******"
        ) ||
        transport_pointer->queued.requests[1].body.size() != 7 ||
        !has_header(
            transport_pointer->queued.requests[1],
            "Content-Range: bytes 327680-327686/327687"
        ) ||
        checkpoints.size() != 1 ||
        checkpoints[0].completed_bytes != chunk_size ||
        checkpoints[0].expiration != "2099-10-05T10:00:00Z") {
        return fail("Graph upload session did not resume from server offset");
    }

    auto backward_transport = std::make_unique<
        FakeTransport>(std::deque<onedrive::http::HttpResult>{
        onedrive::http::HttpResponse{
            .status_code = 200,
            .body =
                R"json({"expirationDateTime":"2099-10-05T10:00:00Z","nextExpectedRanges":["0-"]})json",
        },
    });
    auto* backward_pointer = backward_transport.get();
    onedrive::graph::MicrosoftGraphClient backward_client{
        wrap_transport(std::move(backward_transport)),
        wrap_token_store(std::make_unique<FakeTokenStore>(std::nullopt)),
        auth_options(),
        {
            .drive_id = "me",
            .endpoint = "https://graph.example.test/v1.0",
            .simple_upload_threshold_bytes = 1,
            .upload_chunk_size_bytes = chunk_size,
        },
    };
    try {
        static_cast<void>(backward_client.upload_file(
            "Folder/large.bin",
            std::nullopt,
            "",
            source,
            onedrive::graph::UploadSession{
                .upload_url = "https://upload.example.test/backward",
                .expiration = "2099-10-05T09:00:00Z",
                .completed_bytes = chunk_size,
            }
        ));
        return fail("Graph upload session accepted a backward offset");
    } catch (const std::runtime_error& error) {
        if (!std::string_view{error.what()}.contains("moved backward")) {
            return fail("Graph upload session reported the wrong offset error");
        }
    }
    if (backward_pointer->queued.requests.size() != 1) {
        return fail("Graph upload continued after a backward offset");
    }

    auto checkpoint_transport = std::make_unique<
        FakeTransport>(std::deque<onedrive::http::HttpResult>{
        onedrive::http::HttpResponse{
            .status_code = 200,
            .body =
                R"json({"expirationDateTime":"2099-10-05T10:00:00Z","nextExpectedRanges":["327680-"]})json",
        },
    });
    auto* checkpoint_pointer = checkpoint_transport.get();
    onedrive::graph::MicrosoftGraphClient checkpoint_client{
        wrap_transport(std::move(checkpoint_transport)),
        wrap_token_store(std::make_unique<FakeTokenStore>(std::nullopt)),
        auth_options(),
        {
            .drive_id = "me",
            .endpoint = "https://graph.example.test/v1.0",
            .simple_upload_threshold_bytes = 1,
            .upload_chunk_size_bytes = chunk_size,
        },
    };
    try {
        static_cast<void>(checkpoint_client.upload_file(
            "Folder/large.bin",
            std::nullopt,
            "",
            source,
            onedrive::graph::UploadSession{
                .upload_url = "https://upload.example.test/checkpoint",
                .expiration = "2099-10-05T09:00:00Z",
                .completed_bytes = 0,
            },
            [](const onedrive::graph::UploadSession&) {
                throw std::runtime_error{"simulated checkpoint failure"};
            }
        ));
        return fail("Graph upload ignored a checkpoint failure");
    } catch (const std::runtime_error& error) {
        if (!std::string_view{error.what()}.contains(
                "simulated checkpoint failure"
            )) {
            return fail("Graph upload replaced the checkpoint error");
        }
    }
    if (checkpoint_pointer->queued.requests.size() != 1) {
        return fail("Graph upload sent data after a checkpoint failure");
    }

    auto fragment_checkpoint_transport = std::make_unique<
        FakeTransport>(std::deque<onedrive::http::HttpResult>{
        onedrive::http::HttpResponse{
            .status_code = 200,
            .body = R"({"token_type":"Bearer","expires_in":3600,)"
                    R"("access_token":"access-secret"})",
        },
        onedrive::http::HttpResponse{
            .status_code = 200,
            .body =
                R"json({"uploadUrl":"https://upload.example.test/fragment-checkpoint","expirationDateTime":"2099-10-05T09:00:00Z","nextExpectedRanges":["0-"]})json",
        },
        onedrive::http::HttpResponse{
            .status_code = 202,
            .body =
                R"json({"expirationDateTime":"2099-10-05T10:00:00Z","nextExpectedRanges":["327680-"]})json",
        },
    });
    auto* fragment_checkpoint_pointer = fragment_checkpoint_transport.get();
    onedrive::graph::MicrosoftGraphClient fragment_checkpoint_client{
        wrap_transport(std::move(fragment_checkpoint_transport)),
        wrap_token_store(
            std::make_unique<FakeTokenStore>(std::string{"existing-refresh"})
        ),
        auth_options(),
        {
            .drive_id = "me",
            .endpoint = "https://graph.example.test/v1.0",
            .simple_upload_threshold_bytes = 1,
            .upload_chunk_size_bytes = chunk_size,
        },
    };
    int fragment_checkpoint_count = 0;
    try {
        static_cast<void>(fragment_checkpoint_client.upload_file(
            "Folder/large.bin",
            std::nullopt,
            "",
            source,
            std::nullopt,
            [&](const onedrive::graph::UploadSession&) {
                ++fragment_checkpoint_count;
                if (fragment_checkpoint_count == 2) {
                    throw std::runtime_error{
                        "simulated fragment checkpoint failure"
                    };
                }
            }
        ));
        return fail("Graph upload ignored a fragment checkpoint failure");
    } catch (const std::runtime_error& error) {
        if (!std::string_view{error.what()}.contains(
                "simulated fragment checkpoint failure"
            )) {
            return fail("Graph upload replaced a fragment checkpoint error");
        }
    }
    if (fragment_checkpoint_count != 2 ||
        fragment_checkpoint_pointer->queued.requests.size() != 3) {
        return fail("Graph upload continued after fragment checkpoint failure");
    }

    const auto recreate_source =
        test_directory() / "upload-session-recreate.bin";
    {
        std::ofstream output{recreate_source, std::ios::binary};
        output << "payload";
    }
    const auto verify_recreated = [&](long status_code,
                                      std::string expiration,
                                      std::size_t expected_requests) {
        std::deque<onedrive::http::HttpResult> responses;
        if (status_code != 0) {
            responses.push_back(
                onedrive::http::HttpResponse{
                    .status_code = status_code,
                    .body = R"json({"error":{"message":"expired"}})json",
                }
            );
        }
        responses.push_back(
            onedrive::http::HttpResponse{
                .status_code = 200,
                .body = R"({"token_type":"Bearer","expires_in":3600,)"
                        R"("access_token":"access-secret"})",
            }
        );
        responses.push_back(
            onedrive::http::HttpResponse{
                .status_code = 200,
                .body =
                    R"json({"uploadUrl":"https://upload.example.test/new-session","expirationDateTime":"2099-10-05T11:00:00Z","nextExpectedRanges":["0-"]})json",
            }
        );
        responses.push_back(
            onedrive::http::HttpResponse{
                .status_code = 201,
                .body = uploaded_json,
            }
        );
        auto recreate_transport =
            std::make_unique<FakeTransport>(std::move(responses));
        auto* recreate_pointer = recreate_transport.get();
        onedrive::graph::MicrosoftGraphClient recreate_client{
            wrap_transport(std::move(recreate_transport)),
            wrap_token_store(
                std::make_unique<FakeTokenStore>(std::string{"existing-refresh"}
                )
            ),
            auth_options(),
            {
                .drive_id = "me",
                .endpoint = "https://graph.example.test/v1.0",
                .simple_upload_threshold_bytes = 1,
                .upload_chunk_size_bytes = chunk_size,
            },
        };
        std::vector<onedrive::graph::UploadSession> recreated;
        static_cast<void>(recreate_client.upload_file(
            "Folder/large.bin",
            std::nullopt,
            "",
            recreate_source,
            onedrive::graph::UploadSession{
                .upload_url = "https://upload.example.test/old-session",
                .expiration = std::move(expiration),
                .completed_bytes = 0,
            },
            [&](const onedrive::graph::UploadSession& checkpoint) {
                recreated.push_back(checkpoint);
            }
        ));
        return recreate_pointer->queued.requests.size() == expected_requests &&
               recreate_pointer->queued.requests.back().body.size() == 7 &&
               recreated.size() == 1 && recreated[0].completed_bytes == 0 &&
               recreated[0].upload_url ==
                   "https://upload.example.test/new-session";
    };
    if (!verify_recreated(404, "2099-10-05T09:00:00Z", 4) ||
        !verify_recreated(410, "2099-10-05T09:00:00Z", 4) ||
        !verify_recreated(0, "2000-10-05T09:00:00Z", 3)) {
        return fail("Graph upload did not recreate an unusable session");
    }
    return EXIT_SUCCESS;
}
int test_invalid_upload_session_responses() {
    constexpr std::size_t chunk_size = 320U * 1024U;
    const auto source = test_directory() / "invalid-upload-session.bin";
    {
        std::ofstream output{source, std::ios::binary};
        output << std::string(chunk_size + 1U, 'x');
    }
    const auto expect_failure = [&](std::string session_body,
                                    std::optional<std::string> fragment_body,
                                    std::string_view expected_message) {
        std::deque<onedrive::http::HttpResult> responses{
            onedrive::http::HttpResponse{
                .status_code = 200,
                .body = R"({"token_type":"Bearer","expires_in":3600,)"
                        R"("access_token":"access-secret",)"
                        R"("refresh_token":"existing-refresh"})",
            },
            onedrive::http::HttpResponse{
                .status_code = 200,
                .body = std::move(session_body),
            },
        };
        if (fragment_body) {
            responses.push_back(
                onedrive::http::HttpResponse{
                    .status_code = 202,
                    .body = std::move(*fragment_body),
                }
            );
        }
        onedrive::graph::MicrosoftGraphClient client{
            wrap_transport(
                std::make_unique<FakeTransport>(std::move(responses))
            ),
            wrap_token_store(
                std::make_unique<FakeTokenStore>(std::string{"existing-refresh"}
                )
            ),
            auth_options(),
            {
                .drive_id = "me",
                .endpoint = "https://graph.example.test/v1.0",
                .simple_upload_threshold_bytes = 1,
                .upload_chunk_size_bytes = chunk_size,
            },
        };
        try {
            static_cast<void>(
                client.upload_file("invalid.bin", std::nullopt, "", source)
            );
        } catch (const std::runtime_error& error) {
            return std::string_view{error.what()}.contains(expected_message);
        }
        return false;
    };
    if (!expect_failure(
            R"json({"uploadUrl":"http://upload.example.test/session","expirationDateTime":"2026-10-05T09:00:00Z","nextExpectedRanges":["0-"]})json",
            std::nullopt,
            "unsafe upload session URL"
        ) ||
        !expect_failure(
            R"json({"uploadUrl":"https://upload.example.test/session bad","expirationDateTime":"2026-10-05T09:00:00Z","nextExpectedRanges":["0-"]})json",
            std::nullopt,
            "unsafe upload session URL"
        ) ||
        !expect_failure(
            R"json({"uploadUrl":"https://user@upload.example.test/session","expirationDateTime":"2026-10-05T09:00:00Z","nextExpectedRanges":["0-"]})json",
            std::nullopt,
            "unsafe upload session URL"
        ) ||
        !expect_failure(
            R"json({"uploadUrl":"https://upload.example.test/session","expirationDateTime":"2026-10-05T09:00:00Z"})json",
            std::nullopt,
            "missing nextExpectedRanges"
        ) ||
        !expect_failure(
            R"json({"uploadUrl":"https://upload.example.test/session","expirationDateTime":"2026-10-05T09:00:00Z","nextExpectedRanges":[0]})json",
            std::nullopt,
            "non-string range"
        ) ||
        !expect_failure(
            R"json({"uploadUrl":"https://upload.example.test/session","expirationDateTime":"2026-10-05T09:00:00Z","nextExpectedRanges":["invalid-"]})json",
            std::nullopt,
            "invalid nextExpectedRanges entry"
        ) ||
        !expect_failure(
            R"json({"uploadUrl":"https://upload.example.test/session","expirationDateTime":"2026-10-05T09:00:00Z","nextExpectedRanges":["0-999999"]})json",
            std::nullopt,
            "invalid upload range"
        ) ||
        !expect_failure(
            R"json({"uploadUrl":"https://upload.example.test/session","expirationDateTime":"2026-10-05T09:00:00Z","nextExpectedRanges":["0-"]})json",
            R"json({"nextExpectedRanges":["0-"]})json",
            "non-contiguous range"
        ) ||
        !expect_failure(
            R"json({"uploadUrl":"https://upload.example.test/session","expirationDateTime":"2026-10-05T09:00:00Z","nextExpectedRanges":["0-"]})json",
            R"json({"nextExpectedRanges":["999999-"]})json",
            "out-of-range upload offset"
        )) {
        return fail("invalid Graph upload session response was accepted");
    }
    return EXIT_SUCCESS;
}

} // namespace

int main() {
    if (const int result = test_simple_file_uploads(); result != EXIT_SUCCESS) {
        return result;
    }
    if (const int result =
            test_simple_upload_cancellation_is_not_a_resource_failure();
        result != EXIT_SUCCESS) {
        return result;
    }
    if (const int result = test_upload_session_cancellation_stages();
        result != EXIT_SUCCESS) {
        return result;
    }
    if (const int result = test_upload_sessions(); result != EXIT_SUCCESS) {
        return result;
    }
    if (const int result = test_upload_session_resume();
        result != EXIT_SUCCESS) {
        return result;
    }
    if (const int result = test_invalid_upload_session_responses();
        result != EXIT_SUCCESS) {
        return result;
    }
    return EXIT_SUCCESS;
}
