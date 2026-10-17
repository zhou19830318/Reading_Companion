"""Redaction filter tests: keys are masked, ordinary words are untouched."""

from __future__ import annotations

import logging

from relay.logging_setup import RedactFilter


def clean(msg: str) -> str:
    record = logging.LogRecord("t", logging.INFO, __file__, 1, msg, (), None)
    RedactFilter().filter(record)
    return record.getMessage()


def test_ordinary_words_ending_in_sk_are_not_redacted():
    msg = "asr task-finished timeout; using last result"
    assert clean(msg) == msg


def test_bearer_token_is_masked_entirely():
    assert clean("Authorization: Bearer sk-abcdef1234567890") == "Authorization: Bearer ***"


def test_api_key_forms_are_masked_entirely():
    assert clean("DASHSCOPE_API_KEY=sk-abcdef1234567890") == "DASHSCOPE_API_KEY=***"
    assert clean("api key: sk-abcdef1234567890") == "api key: ***"


def test_sk_shaped_word_tail_is_left_alone():
    msg = "asr task-finished12345678 timeout"
    assert clean(msg) == msg
