"""Reading Companion Voice Relay.

ESP32 device -> WSS /api/v1/voice/stream -> streaming ASR -> streaming LLM.

Package layout follows docs/Reading_Companion_realtime_agent .md section 7:
    relay/
      main.py protocol.py session.py auth.py metrics.py
      providers/{asr_*, llm_*, openclaw}.py
      tests/
"""

__version__ = "0.1.0"
SERVICE_NAME = "voice-relay"
