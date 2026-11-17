# Okou Zero HTTP protocol

SPDX-License-Identifier: MIT

This C++17 client is independently implemented from public wire-contract facts. No BSL implementation is incorporated. Reference checkout inspected: `30ac1c7c85be3d065b9330d90b86c14c3c5d6b0a`; authoritative contract modules are `chat-threads`, `chat-event-rows`, `chat-event-send`, `projects`, `voice-io-transcribe`, and `voice-io-polish` under `turbo/packages/api-contracts/src/contracts`. iOS protocol research supplies interoperability observations, not copied algorithms.

## Authentication and transport

`Http` owns TLS validation, cancellation, response-body/decompression limits and transport errors. API requests use the configured API origin and OAuth bearer, never impersonate iOS and never invent an organization header. JSON writes explicitly set `Content-Type: application/json`. The OAuth token must carry the signed user and organization context. Required scopes are `user:org:read`, `okou:chat:read`, `okou:chat:send`, `okou:project:read`, and `okou:voice:transcribe` for the respective operations. Provider registration, granted scopes, device grant support and a deployed compatible backend remain prerequisites, not properties proven by these local tests.

Signed snapshot URLs are passed byte-for-byte, including their query signatures, to `Http` **without bearer or client identity headers**. Production requires HTTPS; userinfo, fragments, backslashes and CR/LF are rejected. Redirects are not followed. Errors do not include response bodies, prompts, tokens or signed URLs. Unit fixtures alone use the explicit environment gate `OKOU_ZERO_TEST_ALLOW_LOOPBACK_HTTP=1` and numeric `127.0.0.1:<port>` HTTP storage/API origins. Do not enable that gate on a deployed device. Tests exercise actual libcurl HTTP traffic with that gate, not production certificate trust.

## Thread list and projects

1. `GET /api/chat-threads/snapshot` returns exactly one of `{url,latestEventId,latestSeqId}` or `{chatThreads:[],latestEventId:null,latestSeqId:null}`. A remote archive is JSON `{chatThreads:[...]}`.
2. `GET /api/chat-threads/events`, omitting the query for an empty baseline; otherwise `?sinceSeqId=N`. Validate strictly increasing sequence IDs and unique event IDs. Apply lifecycle changes, deletion, archive/visibility, title, agent reassignment, mute and project membership. Continue while `hasMore`; an empty page with `hasMore:true` is invalid.
3. `GET /api/projects` returns `{projects:[{id,name,...}]}`. No project snapshot/event protocol is required for this bounded, fresh list.
4. Default `list(true)` additionally reads `GET /api/indicators`; `list(false)` deliberately does **not** read it and leaves active/unread false for the caller to merge previously confirmed presentation by thread ID. A `threadListChanged` invalidation must call this metadata-only form. A `chatIndicatorsChanged` invalidation calls the independent `indicators()` method.

Indicators are bounded presentation hints, never evidence of a Run's terminal outcome. An active indicator is not unread; mute suppresses unread but not active. Indicators cannot create list membership. Missing entries do not prove completion or complete read coverage.

Each call builds a fresh validated baseline, so there is no persistent cursor/cache shared between accounts or threads. The list cursor is independent of the history cursor. A 410 rebuilds the baseline once; another 410 fails rather than looping. The returned list is the 128 newest visible, nonarchived threads and up to 128 projects. The reconstruction has a 4,096-thread/project and 8,192-event ceiling and fails explicitly beyond those limits. It does not silently call a truncated reconstruction complete. Pin ordering is not implemented; this device view sorts by activity.

## History, revoke and presentation

- `GET /api/chat-threads/<id>/event-snapshot` returns `{url,lastSeqId,lastEventId}`. Only HTTP 404 with canonical nested `{error:{code:"CHAT_EVENT_SNAPSHOT_NOT_FOUND"}}` means an empty baseline. Other 404s propagate.
- A remote history archive is UTF-8 NDJSON, newline-terminated if nonempty. Its final sequence/event identity must match the metadata pair. Gzip content encoding is decoded by `Http`.
- Follow `GET /api/chat-threads/<id>/event-rows?sinceSeqId=N&limit=50[&sinceEventId=UUID]`. Positive sequence cursors always carry the matching event ID; zero cursors omit it. Validate requested thread identity, positive strictly increasing sequences, unique IDs, the final page cursor pair, page size, and nonempty progress on `hasMore:true`.
- A fresh baseline gets one extra confirming tail read after exhaustion. A 410 permits at most one baseline replacement. A second expiration, malformed snapshot/page or an exhausted 8,192-event bound fails closed.
- At most the latest 200 raw-row presentation candidates are retained. Each text is capped to 32 KiB without splitting UTF-8; user documents retain at most 128 text/unsupported-marker parts and no attachment/tool metadata. Payload fields irrelevant to display are discarded. HTTP bodies are separately capped in `Http`; the implementation is **not streaming JSON/NDJSON over the network**. Oversized production histories may therefore require Okou Web.
- `input.*` text parts render as user text. Nontext parts show safe Web-review guidance. `output.message.payload.content` is canonically a plain Markdown string (verified against `ChatEventProjection.swift` and `chat-event-rows.test.ts`), not a mandatory JSON-block envelope. Markdown links, lists and ordinary JSON/code answers remain text. For compatibility, typed JSON text blocks (`type:text,text`) and assistant/message envelopes extract only allowlisted text; unknown typed blocks, tool/action/approval/permission objects are replaced by safe review guidance and their arguments are never rendered. Plain action links additionally show Web-review guidance. `output.error` gets a generic Web-review error, not provider diagnostics. Tool/usage details are not displayed. Unsupported action/approval/permission event types display review guidance and have no execution path.
- `control.revoke.revokesEventId` removes the matching retained message. The `run.completed`, `run.failed` and `run.cancelled` markers affect only messages with the **same nonempty runId**; unmatched markers cannot establish a result. Window eviction can remove evidence, in which case uncertainty is retained rather than inferred from indicators.

## Read watermark

`POST /api/chat-threads/<id>/mark-read` has **no request body**. Its canonical response carries `lastReadAt` (nullable). The client never substitutes a local timestamp and does not optimistically clear unread flags. The caller refreshes indicators/read state after confirmation and serializes HTTP mutations and presentation publication. Realtime token access may be separately serialized by the owner. No Client method is independently thread-safe.

## Text send and uncertain writes

Sending is explicit and separate from dictation. A metadata preflight `GET /api/chat-threads/<id>/metadata` validates the thread and agent and preserves `computerUseHostId` and `cloudBrowserEnabled`. Then `POST /api/chat/events` carries:

```json
{
  "agentId": "uuid",
  "threadId": "uuid",
  "prompt": "editable user text",
  "clientEventId": "stable UUID v4",
  "chatThreadSortEventId": "stable UUID v4",
  "userMessage": {"version": 1, "parts": [{"type": "text", "text": "editable user text"}]},
  "hasTextContent": true,
  "computerUseHostId": null,
  "cloudBrowserEnabled": false
}
```

Both operation identities are generated before dispatch using OpenSSL `RAND_bytes` with UUID v4/variant bits. There is no additional UUID library. The pending identity record contains **no prompt, audio or bearer**, and its filename is SHA-256 scoped by API origin, user, organization and thread. Creation uses `O_EXCL|O_NOFOLLOW`, mode 0600, file fsync and directory fsync before dispatch. Unsafe existing records fail closed. The owning config/auth code must provide a private data directory.

A successful matching `{threadId}` response confirms API acceptance, not delivery/completion, and removes the pending record. Definite 4xx rejections clear it, except 408/409 are conservatively uncertain. Network failures, dropped responses, 5xx, invalid success bodies and ambiguous cancellations retain it and return `{client_event_id,uncertain:true}`. There is **no automatic resubmission**, even with the same UUID. Existing pending state blocks all later sends on that scoped thread. This is stricter than assuming backend idempotency.

On restart or selection, call `history(thread)` and then `has_pending_send(thread)` before enabling Enter. History scans **all validated raw rows**, not merely visible messages or a search index. It clears pending state only when the matching canonical `input.prompt` event ID is observed and the entire baseline/tail read succeeds. Missing evidence never authorizes a resend. A malformed later cursor cannot clear the pending record. A compacted history beyond client limits remains blocked and requires Web review; do not delete its ledger to force a retry. Logout/account changes must not indiscriminately erase uncertain-send records.

## Voice

`transcribe` takes PCM16 mono samples at **16 kHz** and an overlap of at most 2 seconds, not longer than the audio. This implementation caps one upload at 60 seconds (stricter than the server's 75-second segment ceiling). Empty PCM returns locally. It checks `GET /api/voice-io/quota` before every upload and requires `allowed:true`.

`POST /api/voice-io/transcribe/segment` multipart fields:

- `file`: binary RIFF/WAVE PCM16 mono 16 kHz, filename `segment.wav`, MIME `audio/wav`.
- `options`: JSON `{previousTranscript,overlapDurationSeconds,totalDurationSeconds}`. Previous text is the last 1,000 UTF-16 units without splitting UTF-8 characters. This API signature uses the current segment duration for totalDurationSeconds; cumulative session duration is not represented by the shared API.
- `editorContext`: JSON `{before:"",selected:"",after:""}`. The shared C++ interface has no editor-selection parameters; no context is invented.

204 returns empty text. A normal response uses `transcript`. Quota/transport errors propagate without automatic upload retry. `polish` trims/drops empty segments, packs ordered text into at most 128 entries (overflow entries join the last), and posts exactly `{segments:[...]}` to `/api/voice-io/polish/segments`; empty input skips the request. A 128 KiB aggregate safety bound is stricter than the backend's character ceiling. The returned `{text}` populates an editable draft. Neither transcription nor polish sends chat text or attaches audio.

## Realtime auth

`POST /api/realtime/token` sends `{}` and returns the complete Ably **TokenRequest JSON**, not a plain token or OAuth bearer. Required signed fields are validated. The realtime owner is responsible for capability-compatible channel subscriptions and invalidations. The OAuth backend grants chat-scoped channels, not the full iOS session channel set. Do not subscribe to user-global or foreground/presence channels unless the actual capability permits them.

## Local verification

`tests/client_test.cpp` is a standalone throwing-check executable with a local loopback HTTP server, actual `Http` requests and real WAV encoding. It verifies canonical paths/bodies, signed query preservation, bearer/client-header isolation, list tail/metadata-only reads, history tail/revocation, malformed identity/order/cursor/progress, bounded 410 recovery, matching-run terminal status, canonical Markdown/ordinary-JSON preservation and safe text-block/action presentation, wrong success-response identity, matching input followed by an invalid cursor not clearing pending, mute preservation, uncertain send identity persistence and restart reconciliation, absence-not-retry behavior, 400/409 policy, multipart JSON and WAV bytes, quota gating, 204, polish, mark-read, complete TokenRequest, unsafe storage URLs and secure UUID shape.

Example standalone build from repository root (Linux development packages required):

```sh
g++ -std=c++17 -Iinclude -Wall -Wextra -Werror \
  tests/client_test.cpp src/client.cpp src/http.cpp src/audio.cpp \
  -lcurl -lcrypto -lasound -pthread -o /tmp/okou-client-test
/tmp/okou-client-test
```

No live Okou API, real user send, real voice upload, production OAuth or storage TLS credentials are used by these tests.
