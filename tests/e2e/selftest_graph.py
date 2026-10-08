from __future__ import annotations

import io
import json
from pathlib import Path
import time
from urllib import error as url_error, request

from . import graph as graph_module
from .base import E2EError
from .graph import GraphMoveFixture, json_request, refresh_graph_access_token


def self_test_graph(state: Path, token: Path) -> None:
    class FakeResponse:
        def __init__(
            self,
            status: int,
            body: bytes,
            headers: dict[str, str] | None = None,
        ) -> None:
            self.status = status
            self._body = body
            self.headers = headers or {}

        def __enter__(self) -> FakeResponse:
            return self

        def __exit__(self, *arguments: object) -> None:
            return None

        def read(self) -> bytes:
            return self._body

    urlopen_results: list[object] = [
        FakeResponse(200, b'{"ok":true}'),
        url_error.HTTPError(
            "https://graph.example.test/retry",
            429,
            "throttled",
            {"Retry-After": "0"},
            io.BytesIO(b"{}"),
        ),
        FakeResponse(204, b""),
        url_error.HTTPError(
            "https://graph.example.test/fail",
            400,
            "bad request",
            {},
            io.BytesIO(b"{}"),
        ),
    ]

    def fake_urlopen(
        graph_request: request.Request,
        timeout: int,
    ) -> object:
        del graph_request, timeout
        result = urlopen_results.pop(0)
        if isinstance(result, BaseException):
            raise result
        return result

    original_urlopen = request.urlopen
    original_sleep = time.sleep
    request.urlopen = fake_urlopen
    time.sleep = lambda delay: None
    try:
        response = json_request(
            "https://graph.example.test/success",
            "POST",
            access_token="access-token",
            body=b"{}",
            content_type="application/json",
            expected_statuses={200},
        )
        if response != {"ok": True}:
            raise E2EError("JSON request success self-test failed")
        if (
            json_request(
                "https://graph.example.test/retry",
                "DELETE",
                expected_statuses={204},
            )
            is not None
        ):
            raise E2EError("JSON request retry self-test failed")
        try:
            json_request(
                "https://graph.example.test/fail",
                "GET",
                expected_statuses={200},
            )
            raise E2EError("unexpected Graph HTTP status was accepted")
        except E2EError as graph_error:
            if "unexpected HTTP status 400" not in str(graph_error):
                raise
    finally:
        request.urlopen = original_urlopen
        time.sleep = original_sleep

    requests: list[tuple[str, str, dict[str, object]]] = []

    def fake_json_request(
        url: str,
        method: str,
        **kwargs: object,
    ) -> object | None:
        requests.append((url, method, kwargs))
        if url.endswith("/oauth2/v2.0/token"):
            return {
                "access_token": "access-token",
                "refresh_token": "rotated-token",
            }
        if method == "GET" and url.endswith("/root:/root/missing.bin"):
            raise E2EError(
                "GET request returned unexpected HTTP status 404"
            )
        if method == "DELETE":
            return None
        return {"id": f"item-{len(requests)}"}

    original_json_request = graph_module.json_request
    graph_module.json_request = fake_json_request
    try:
        access_token = refresh_graph_access_token(
            {
                "auth": {
                    "application_id": "application",
                    "tenant_id": "tenant/id",
                    "endpoint": "https://login.example.test/",
                    "scopes": [
                        "User.Read",
                        "Files.ReadWrite",
                        "offline_access",
                    ],
                }
            },
            state,
        )
        if (
            access_token != "access-token"
            or token.read_text(encoding="utf-8") != "rotated-token"
        ):
            raise E2EError("token refresh self-test failed")

        graph_fixture = GraphMoveFixture(
            "https://graph.example.test/v1.0/",
            "drive/id",
            access_token,
        )
        graph_root_id = graph_fixture.create_folder("root")
        graph_child_id = graph_fixture.create_folder(
            "child",
            graph_root_id,
        )
        graph_file_id = graph_fixture.upload_file(
            graph_child_id,
            "file name.bin",
            b"contents",
        )
        graph_fixture.move_item(
            graph_file_id,
            graph_root_id,
            "renamed.bin",
        )
        looked_up = graph_fixture.item_by_path(
            Path("root/renamed.bin")
        )
        if looked_up.get("id") is None:
            raise E2EError("Graph fixture path lookup self-test failed")
        if not graph_fixture.item_exists(Path("root/renamed.bin")):
            raise E2EError("Graph fixture existence self-test failed")
        if graph_fixture.item_exists(Path("root/missing.bin")):
            raise E2EError("Graph fixture accepted a missing item")
        graph_fixture.replace_file(graph_file_id, b"replacement")
        graph_fixture.delete_root()
        if graph_fixture.root_id is not None:
            raise E2EError("Graph fixture cleanup self-test failed")
    finally:
        graph_module.json_request = original_json_request

    graph_calls = requests[1:]
    if [call[1] for call in graph_calls] != [
        "POST",
        "POST",
        "PUT",
        "PATCH",
        "GET",
        "GET",
        "GET",
        "PUT",
        "DELETE",
    ]:
        raise E2EError("Graph fixture request sequence self-test failed")
    if (
        "/drives/drive%2Fid/root/children" not in graph_calls[0][0]
        or "file%20name.bin:/content" not in graph_calls[2][0]
        or graph_calls[2][2].get("access_token") != "access-token"
    ):
        raise E2EError("Graph fixture URL self-test failed")
    move_body = graph_calls[3][2].get("body")
    if not isinstance(move_body, bytes):
        raise E2EError("Graph fixture move body self-test failed")
    move_payload = json.loads(move_body)
    if move_payload != {
        "name": "renamed.bin",
        "parentReference": {"id": graph_root_id},
    }:
        raise E2EError("Graph fixture move payload self-test failed")
