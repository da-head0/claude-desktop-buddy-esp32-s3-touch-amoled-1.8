"""AWS Lambda handler — receives raw 16 kHz mono 16-bit PCM, returns JSON.

The board firmware (`src/voice_stt.cpp`) POSTs the captured audio to the
Function URL with header `x-api-key: <key>`. This handler validates that
key against the `API_KEY` env var, streams the PCM to AWS Transcribe
Streaming, and replies with `{"text": "...", "ms": <round-trip>}`.

Language and sample rate are env-var driven so a single deployment can be
re-targeted without re-zipping (default ko-KR @ 16 kHz to match the
firmware capture format).
"""

from __future__ import annotations

import asyncio
import base64
import json
import os
import time

from amazon_transcribe.client import TranscribeStreamingClient
from amazon_transcribe.handlers import TranscriptResultStreamHandler
from amazon_transcribe.model import TranscriptEvent

LANGUAGE_CODE = os.environ.get("LANGUAGE_CODE", "ko-KR")
SAMPLE_RATE = int(os.environ.get("SAMPLE_RATE", "16000"))
EXPECTED_API_KEY = os.environ.get("API_KEY", "")
REGION = os.environ.get("AWS_REGION", "ap-northeast-2")


class TranscriptCollector(TranscriptResultStreamHandler):
    """Captures finalised (non-partial) transcripts as Transcribe streams."""

    def __init__(self, output_stream):
        super().__init__(output_stream)
        self.parts: list[str] = []

    async def handle_transcript_event(self, transcript_event: TranscriptEvent):
        for result in transcript_event.transcript.results:
            if result.is_partial:
                continue
            for alt in result.alternatives:
                if alt.transcript:
                    self.parts.append(alt.transcript)


async def _transcribe(pcm: bytes) -> str:
    client = TranscribeStreamingClient(region=REGION)
    stream = await client.start_stream_transcription(
        language_code=LANGUAGE_CODE,
        media_sample_rate_hz=SAMPLE_RATE,
        media_encoding="pcm",
    )

    async def feed():
        # ~250 ms chunks at 16 kHz mono 16-bit. Transcribe likes a steady
        # cadence; sending the whole buffer in one event sometimes triggers
        # "audio chunk too large" rejections.
        chunk_size = 1024 * 8
        for i in range(0, len(pcm), chunk_size):
            await stream.input_stream.send_audio_event(
                audio_chunk=pcm[i : i + chunk_size]
            )
        await stream.input_stream.end_stream()

    handler = TranscriptCollector(stream.output_stream)
    await asyncio.gather(feed(), handler.handle_events())
    return " ".join(handler.parts).strip()


def _json_response(status: int, payload: dict) -> dict:
    return {
        "statusCode": status,
        "headers": {"content-type": "application/json; charset=utf-8"},
        # ensure_ascii=False so Hangul/emoji travel as UTF-8 bytes — the
        # firmware decodes UTF-8 in `bleHidTypeUtf8`.
        "body": json.dumps(payload, ensure_ascii=False),
    }


def lambda_handler(event, _context):
    # API key check (case-insensitive — Function URL preserves case as
    # sent, but defensive coding is cheap).
    if EXPECTED_API_KEY:
        headers = event.get("headers") or {}
        provided = (
            headers.get("x-api-key")
            or headers.get("X-Api-Key")
            or headers.get("X-API-Key")
            or ""
        )
        if provided != EXPECTED_API_KEY:
            return _json_response(401, {"error": "invalid api key"})

    body = event.get("body") or ""
    if event.get("isBase64Encoded"):
        body = base64.b64decode(body)
    elif isinstance(body, str):
        # Function URL with non-text content-type still falls here when the
        # client sent application/octet-stream without forcing base64.
        body = body.encode("latin-1")

    if not body or len(body) < SAMPLE_RATE:  # < 0.5 s of audio is not worth a round-trip
        return _json_response(400, {"error": "audio too short", "bytes": len(body)})

    t0 = time.time()
    try:
        text = asyncio.run(_transcribe(body))
    except Exception as e:
        return _json_response(502, {"error": "transcribe failed", "detail": str(e)})
    elapsed_ms = int((time.time() - t0) * 1000)

    return _json_response(200, {"text": text, "ms": elapsed_ms})
