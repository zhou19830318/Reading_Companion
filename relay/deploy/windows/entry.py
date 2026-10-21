"""Frozen-app entry point used by the Windows installer / Windows service.

Loads the same key=value env file install.sh writes on Linux, so both
deployments share one configuration format. PyInstaller bundles this script
together with the relay package, uvicorn and every dependency.
"""

from __future__ import annotations

import os
import sys
from pathlib import Path

DEFAULT_ENV_FILE = r"C:\ProgramData\ReadingCompanion\voice-relay.env"


def load_env_file(path: str) -> None:
    file = Path(path)
    if not file.is_file():
        return
    # utf-8-sig tolerates a BOM if the file was edited with Notepad.
    for raw in file.read_text(encoding="utf-8-sig").splitlines():
        line = raw.strip()
        if not line or line.startswith("#") or "=" not in line:
            continue
        key, _, value = line.partition("=")
        key = key.strip()
        if key:
            # Real environment variables win: operators can override the file.
            os.environ.setdefault(key, value.strip())


def main() -> int:
    # Expand %VAR% so a literal "%ProgramData%\..." coming from the service
    # configuration resolves even if the wrapper does not expand it itself.
    env_file = os.path.expandvars(os.environ.get("VOICE_RELAY_ENV_FILE") or DEFAULT_ENV_FILE)
    load_env_file(env_file)
    # Manual runs (python deploy/windows/entry.py) need the relay root on the
    # path; the PyInstaller build already bundles the package.
    if not getattr(sys, "frozen", False):
        package_root = Path(__file__).resolve().parents[2]
        if str(package_root) not in sys.path:
            sys.path.insert(0, str(package_root))
    # Home-LAN defaults: the reader connects from another machine, so the
    # service must not bind loopback only.
    os.environ.setdefault("RELAY_BIND", "0.0.0.0")
    os.environ.setdefault("RELAY_PORT", "8080")

    import uvicorn

    from relay.main import app as relay_app

    uvicorn.run(
        relay_app,
        host=os.environ["RELAY_BIND"],
        port=int(os.environ["RELAY_PORT"]),
        log_level=os.environ.get("RELAY_LOG_LEVEL", "INFO").lower(),
        access_log=False,
    )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
