"""Minimal Gemini generateContent client, stdlib only."""

from __future__ import annotations

import json
import random
import time
import urllib.error
import urllib.request

ENDPOINT = "https://generativelanguage.googleapis.com/v1beta/models/{model}:generateContent"
RETRY_STATUS = {408, 429, 500, 502, 503, 504}

RESPONSE_SCHEMA = {
    "type": "object",
    "properties": {
        "verdict": {
            "type": "string",
            "enum": ["approve", "comment", "request_changes"],
        },
        "summary": {
            "type": "string",
            "description": "Two to five sentences on what the change does and whether it looks correct.",
        },
        "human_review": {
            "type": "string",
            "enum": ["recommended", "optional", "unnecessary"],
            "description": "Whether a human should still read this change.",
        },
        "human_review_reason": {
            "type": "string",
            "description": "One short sentence naming what makes it risky, or why it is safe.",
        },
        "findings": {
            "type": "array",
            "items": {
                "type": "object",
                "properties": {
                    "file": {"type": "string", "description": "Repository-relative path, exactly as in the diff."},
                    "line": {"type": "integer", "description": "Line number in the file after the change."},
                    "end_line": {"type": "integer", "description": "Last line of the range, or the same as line."},
                    "severity": {"type": "string", "enum": ["critical", "major", "minor", "nit"]},
                    "confidence": {"type": "string", "enum": ["high", "medium", "low"]},
                    "title": {"type": "string", "description": "One short sentence naming the defect."},
                    "detail": {"type": "string", "description": "Why it is wrong and what breaks, citing the related code."},
                    "suggestion": {
                        "type": "string",
                        "description": "Replacement source for lines line..end_line, or empty. No fences, no line numbers.",
                    },
                },
                "required": ["file", "line", "end_line", "severity", "confidence", "title", "detail"],
                "propertyOrdering": [
                    "file", "line", "end_line", "severity", "confidence", "title", "detail", "suggestion",
                ],
            },
        },
    },
    "required": ["verdict", "summary", "human_review", "human_review_reason", "findings"],
    "propertyOrdering": ["verdict", "summary", "human_review", "human_review_reason", "findings"],
}

SYSTEM_PROMPT = """\
You are reviewing a pull request for Vita3K, a PlayStation Vita emulator written in C++20.

You are given the diff, the full text of every changed file after the change, and related code that
was not changed: counterpart headers, definitions of the symbols the diff calls into, and existing
callers of the symbols the diff defines. Use that related code. Most real defects in this codebase
are not visible in the diff alone: a changed signature that a caller elsewhere still calls the old
way, a struct field whose size or layout the guest ABI depends on, an enum that another switch
statement must also handle.

What to report:
- Correctness: logic errors, wrong operator, off-by-one, inverted condition, unhandled error return.
- Memory and lifetime: use-after-free, dangling reference, unchecked Ptr<T> translation, buffer
  overrun, misaligned or wrongly sized guest memory access, leaks on an early return.
- Concurrency: data races on emulator state, missing lock, lock ordering, non-atomic access shared
  with the renderer or a guest thread.
- Interface breakage: callers, overrides or switch statements elsewhere that this change invalidates.
- Emulator-specific: wrong PSVita return code or error constant, endianness, guest struct layout or
  field size that must match hardware, an unimplemented path silently returning success.

What NOT to report:
- Formatting, brace placement, include order, or anything clang-format already enforces.
- Restating what the change does, praise, or generic advice ("consider adding tests").
- Speculation about code you were not shown. If you need a file you do not have, say so in the
  summary instead of inventing a finding.
- Pre-existing problems the diff does not touch, unless the change makes them reachable.

Rules for findings:
- `file` must be a path that appears in the diff, spelled exactly as it appears there.
- `line` and `end_line` are line numbers in the file AFTER the change, as shown in the numbered
  listing. Anchor on the line that is actually wrong.
- `confidence` is `high` only when the provided code proves the defect. If you are reasoning about
  code you were not shown, it is `low`.
- `suggestion` is optional. Fill it only when you can give the exact replacement source for lines
  `line` through `end_line`, with correct indentation, no fences and no line numbers. Otherwise "".
- Report nothing rather than padding. An empty `findings` list with `verdict: approve` is a good
  review of a correct change.

Finally, judge in `human_review` whether a person should still read this change:
- `recommended`: it touches guest memory layout, an ABI-visible struct, threading or renderer
  state; or it is large, cross-cutting, or hard to follow; or you were not given enough context
  to be confident. Prefer this whenever you hesitate.
- `optional`: an ordinary change you could follow end to end and believe is correct.
- `unnecessary`: mechanical and free of behaviour change, such as a typo, a comment, or a rename
  with no call site left behind.
Give the one-sentence reason in `human_review_reason`, naming what makes it risky or why it is safe.
"""


class GeminiError(RuntimeError):
    pass


class GeminiUnavailable(GeminiError):
    """Transient: the API was overloaded or rate limited. Not the pull request's fault."""


def _call(api_key: str, model: str, payload: dict, timeout: int) -> dict:
    request = urllib.request.Request(
        ENDPOINT.format(model=model),
        data=json.dumps(payload).encode("utf-8"),
        headers={"Content-Type": "application/json", "x-goog-api-key": api_key},
        method="POST",
    )
    with urllib.request.urlopen(request, timeout=timeout) as response:
        return _parse(json.loads(response.read().decode("utf-8")))


def retry_delay(body: str) -> float | None:
    """Seconds Google asks us to wait, from the RetryInfo it attaches to 429 and 503 responses."""
    try:
        payload = json.loads(body)
    except (json.JSONDecodeError, TypeError):
        return None
    for detail in ((payload.get("error") or {}).get("details") or []):
        value = detail.get("retryDelay") if isinstance(detail, dict) else None
        if isinstance(value, str) and value.endswith("s"):
            try:
                return float(value[:-1])
            except ValueError:
                return None
    return None


def _payload(user_prompt: str, max_output_tokens: int, thinking_level: str) -> dict:
    config = {
        "responseMimeType": "application/json",
        "responseSchema": RESPONSE_SCHEMA,
        "maxOutputTokens": max_output_tokens,
    }
    if thinking_level:
        config["thinkingConfig"] = {"thinkingLevel": thinking_level}
    return {
        "systemInstruction": {"parts": [{"text": SYSTEM_PROMPT}]},
        "contents": [{"role": "user", "parts": [{"text": user_prompt}]}],
        "generationConfig": config,
    }


def generate(
    api_key: str,
    models: list[str],
    user_prompt: str,
    thinking_level: str = "high",
    max_output_tokens: int = 32768,
    timeout: int = 600,
    attempts: int = 2,
    deadline: float | None = None,
) -> dict:
    """Try each model in turn, backing off between attempts, until the deadline runs out."""
    if deadline is None:
        deadline = time.monotonic() + 20 * 60

    last_error = "no attempt was made"
    for model in models:
        level = thinking_level
        attempt = 0
        while attempt < attempts:
            remaining = deadline - time.monotonic()
            if remaining < 30:
                raise GeminiUnavailable(f"out of time; last error: {last_error}")
            advised: float | None = None
            try:
                result = _call(
                    api_key, model, _payload(user_prompt, max_output_tokens, level), min(timeout, int(remaining))
                )
                result["_model"] = model
                result["_thinking_level"] = level
                return result
            except urllib.error.HTTPError as error:
                raw = error.read().decode("utf-8", "replace")
                last_error = f"{model}: HTTP {error.code}: {raw[:400]}"
                # Not every model takes thinkingLevel; that is worth one retry, not a dead end.
                if error.code == 400 and level and "thinking" in raw.lower():
                    print(f"::warning::{model} rejected thinkingLevel, retrying without it")
                    level = ""
                    continue
                if error.code not in RETRY_STATUS:
                    raise GeminiError(last_error) from error
                advised = retry_delay(raw)
                # A 429 means quota, not congestion. Without a usable retry hint it is the daily
                # allowance, which no amount of waiting restores, so stop spending it on this model.
                if error.code == 429 and advised is None:
                    print(f"::warning::{model} is out of quota, moving on")
                    break
            except (urllib.error.URLError, TimeoutError) as error:
                last_error = f"{model}: {error}"
            print(f"::warning::{last_error}")
            attempt += 1
            if attempt >= attempts:
                break
            wait = advised if advised is not None else min(120, 15 * 2 ** (attempt - 1)) + random.uniform(0, 5)
            # An exhausted daily quota outlasts the job; spend the time on the next model instead.
            if wait > deadline - time.monotonic() - 30:
                print(f"::warning::{model} asks for a {wait:.0f}s wait, moving on")
                break
            time.sleep(wait)
    raise GeminiUnavailable(f"every model was unavailable; last error: {last_error}")


def _parse(response: dict) -> dict:
    candidates = response.get("candidates") or []
    if not candidates:
        feedback = response.get("promptFeedback", {})
        raise GeminiError(f"no candidate returned (promptFeedback={feedback})")

    candidate = candidates[0]
    reason = candidate.get("finishReason", "")
    parts = (candidate.get("content") or {}).get("parts") or []
    text = "".join(p.get("text", "") for p in parts if not p.get("thought"))
    if not text.strip():
        raise GeminiError(f"empty response (finishReason={reason})")

    try:
        result = json.loads(text)
    except json.JSONDecodeError as error:
        if reason == "MAX_TOKENS":
            raise GeminiError("response truncated at maxOutputTokens") from error
        raise GeminiError(f"response was not valid JSON (finishReason={reason})") from error

    result["_usage"] = response.get("usageMetadata", {})
    result["_finish_reason"] = reason
    return result
