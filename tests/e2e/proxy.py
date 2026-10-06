from __future__ import annotations

import socket
import threading


class ConnectionDropProxy:
    def __init__(self) -> None:
        self._socket = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
        self._socket.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
        self._socket.bind(("127.0.0.1", 0))
        self._socket.listen()
        self._socket.settimeout(0.1)
        self._stopped = threading.Event()
        self._thread = threading.Thread(target=self._run, daemon=True)
        self.connection_count = 0

    @property
    def url(self) -> str:
        return f"http://127.0.0.1:{self._socket.getsockname()[1]}"

    def __enter__(self) -> ConnectionDropProxy:
        self._thread.start()
        return self

    def __exit__(self, *arguments: object) -> None:
        self._stopped.set()
        self._socket.close()
        self._thread.join(timeout=2)

    def _run(self) -> None:
        while not self._stopped.is_set():
            try:
                connection, _ = self._socket.accept()
            except TimeoutError:
                continue
            except OSError:
                return
            self.connection_count += 1
            connection.close()
