"""Test environment: mock providers, a fixed device token, no external I/O."""

from __future__ import annotations

import os
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))

# Must be set before relay.config first reads the environment.
os.environ.setdefault("RELAY_ENV", "test")
os.environ.setdefault("RELAY_LOG_LEVEL", "WARNING")
os.environ.setdefault("RELAY_DEVICE_TOKEN", "test-device-token")
os.environ.setdefault("RELAY_ADMIN_TOKEN", "test-admin-token")
os.environ.setdefault("ALLOWED_DEVICE_IDS", "debug-client")
os.environ.setdefault("ASR_PROVIDER", "mock")
os.environ.setdefault("LLM_PROVIDER", "mock")
os.environ.setdefault("MOCK_ASR_PARTIAL_INTERVAL_MS", "200")
os.environ.setdefault("MOCK_ASR_FINAL_DELAY_MS", "50")
os.environ.setdefault("MOCK_LLM_DELTA_INTERVAL_MS", "10")
os.environ.setdefault("MOCK_LLM_DELTA_CHARS", "4")
