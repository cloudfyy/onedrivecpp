from __future__ import annotations


class E2EProgress:
    def __init__(self, total: int) -> None:
        if total <= 0:
            raise ValueError("E2E progress total must be positive")
        self._total = total
        self._current = 0

    def step(self, description: str) -> None:
        if self._current >= self._total:
            raise RuntimeError("E2E progress exceeded its declared total")
        self._current += 1
        percentage = self._current * 100 // self._total
        print(
            f"[E2E {self._current}/{self._total} {percentage:3d}%] "
            f"{description}",
            flush=True,
        )
