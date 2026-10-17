"""Structured JSON logging with light redaction.

python-json-logger formats every record; a filter scrubs anything that looks
like a bearer token or API key from the message, so a careless log call can
never leak the Key (docs sections 9 and 17.14).
"""

from __future__ import annotations

import logging
import re
import sys

try:  # python-json-logger >= 3.1
    from pythonjsonlogger.json import JsonFormatter
except ImportError:  # pragma: no cover - 2.x fallback
    from pythonjsonlogger.jsonlogger import JsonFormatter

_REDACT_PATTERNS = [
    re.compile(r"(Bearer\s+)[A-Za-z0-9._\-]+", re.IGNORECASE),
    re.compile(r"(api[-_ ]?key[\"'\s:=]+)[A-Za-z0-9._\-]+", re.IGNORECASE),
    # \b keeps words like "task-finished" from matching as an sk- key.
    re.compile(r"\b(sk-)[A-Za-z0-9._\-]{8,}"),
]


class RedactFilter(logging.Filter):
    def filter(self, record: logging.LogRecord) -> bool:
        try:
            msg = record.getMessage()
        except Exception:  # pragma: no cover - malformed record
            return True
        clean = msg
        for pattern in _REDACT_PATTERNS:
            clean = pattern.sub(lambda m: f"{m.group(1)}***", clean)
        if clean != msg:
            record.msg = clean
            record.args = ()
        return True


def setup_logging(level: str = "INFO") -> None:
    root = logging.getLogger()
    if root.handlers:
        root.handlers.clear()
    handler = logging.StreamHandler(sys.stdout)
    handler.setFormatter(
        JsonFormatter(
            "%(asctime)s %(levelname)s %(name)s %(message)s",
            rename_fields={"asctime": "ts", "levelname": "level", "name": "logger"},
        )
    )
    handler.addFilter(RedactFilter())
    root.addHandler(handler)
    root.setLevel(getattr(logging, level.upper(), logging.INFO))
    # uvicorn access logs stay at warning: request lines would echo query
    # strings we do not need and the JSON formatter would duplicate them.
    logging.getLogger("uvicorn.access").setLevel(logging.WARNING)
