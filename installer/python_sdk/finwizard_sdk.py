"""
finwizard_sdk.py
-----------------
Helper library for FinWizard plugin authors.

Two separate channels, don't mix them:

  1. FILES (input.json / output.json) - the plugin's parameters and its
     final result. Use load_input() / write_success() / write_error(),
     or just @entrypoint which handles all of that for you.

  2. BRIDGE (stdin/stdout, live) - small control messages *during*
     execution: log lines, progress updates, small data queries. Use
     core.log() / core.update_progress() / core.get_db_data(), or the
     log() shortcut below. Never send file contents over this channel -
     pass a path in input.json / outputPath in the result instead.

Usage in a plugin:

    import finwizard_sdk as fw

    @fw.entrypoint
    def run(params):
        fw.log("Starting import...")
        fw.core.update_progress(10, "Reading source file")
        rows = do_the_work(params["source_file"])
        return {"rows_imported": rows}

    if __name__ == "__main__":
        run()
"""

import sys
import json
import itertools
import functools
import traceback
from pathlib import Path


class PluginError(Exception):
    """Raise inside a plugin to fail cleanly with a specific message.
    Keyword arguments are merged into the output.json result."""
    def __init__(self, message, **extra):
        super().__init__(message)
        self.message = message
        self.extra = extra


class _StdoutProxy:
    """Быстрый прокси: собираем вывод в буфер и отправляем пачками."""
    def __init__(self, bridge):
        self._bridge = bridge
        self._buf = []

    def write(self, s):
        if s == '\n':
            self.flush()
        else:
            self._buf.append(s)

    def flush(self):
        if self._buf:
            line = ''.join(self._buf).rstrip()
            if line:
                self._bridge.log(line)
            self._buf.clear()

    def __del__(self):
        self.flush()


class EngineBridge:
    def __init__(self):
        self._real_stdout = sys.stdout
        self._call_id = itertools.count(1)
        sys.stdout = _StdoutProxy(self)   # ← важное изменение

    def _write_payload(self, payload: dict):
        self._real_stdout.write(json.dumps(payload) + "\n")
        self._real_stdout.flush()

    def _send_notification(self, action: str, **kwargs):
        """Fire-and-forget notification (no 'id', no response expected)."""
        payload = {"action": action, "params": kwargs}
        self._write_payload(payload)

    def _send_request(self, action: str, **kwargs):
        """Synchronous round-trip request (expects response with matching id)."""
        req_id = next(self._call_id)
        payload = {"id": req_id, "action": action, "params": kwargs}
        self._write_payload(payload)

        line = sys.stdin.readline()
        if not line:
            raise RuntimeError("Engine disconnected")

        response = json.loads(line)
        if response.get("id") != req_id:
            raise RuntimeError("Protocol desync")
        if not response.get("success"):
            raise RuntimeError(response.get("error", "Unknown error"))
        return response.get("result")

    def log(self, message: str):
        """Send a line to the host's log window (fire-and-forget)."""
        self._send_notification("log", message=str(message))

    def update_progress(self, percent: int, status_text: str = ""):
        """Update the progress bar in the host UI (fire-and-forget). percent: 0-100."""
        self._send_notification("update_progress", percent=percent, text=status_text)

    def get_db_data(self, query_type: str, **params):
        """Ask the host for small structured data (blocking RPC)."""
        return self._send_request("get_db_data", type=query_type, **params)

core = EngineBridge()


def load_input():
    """Read and parse input.json. Path is argv[1], per the host's contract."""
    if len(sys.argv) < 3:
        raise RuntimeError(
            "Expected two arguments: <input_path> <output_path>. "
            "Plugins are launched by the host, not meant to be run bare."
        )
    with Path(sys.argv[1]).open(encoding="utf-8") as f:
        return json.load(f)


def _output_path() -> Path:
    return Path(sys.argv[2])


def write_success(message: str = "", output_path: str = "", **extra):
    """Write a successful result to output.json. `output_path` is a file
    the plugin produced (e.g. a report) - unrelated to the output.json
    path itself."""
    result = {"success": True, "message": message, "outputPath": output_path}
    result.update(extra)
    _output_path().write_text(json.dumps(result, ensure_ascii=False), encoding="utf-8")


def write_error(error, **extra):
    """Write a failed result to output.json."""
    result = {"success": False, "error": str(error)}
    result.update(extra)
    _output_path().write_text(json.dumps(result, ensure_ascii=False), encoding="utf-8")


def log(*args):
    """Shortcut for core.log() - send a status line to the host's log window."""
    core.log(" ".join(str(a) for a in args))


def entrypoint(func):
    """Decorator for a plugin's main function: func(params: dict) -> dict | None.

    Handles load_input/write_success/write_error and exception -> failure
    result automatically, so plugin code is just business logic."""
    @functools.wraps(func)
    def wrapper():
        try:
            params = load_input()
            result = func(params) or {}
            message = result.pop("message", "")
            output_path = result.pop("outputPath", "")
            write_success(message=message, output_path=output_path, **result)
        except PluginError as e:
            write_error(e.message, **e.extra)
            sys.exit(1)
        except Exception as e:
            traceback.print_exc()  # to real stderr, captured by host on non-zero exit
            write_error(f"{type(e).__name__}: {e}")
            sys.exit(1)
    return wrapper
