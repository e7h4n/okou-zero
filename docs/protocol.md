# Cardputer Zero client protocol

## Fixed conversation

The first launch for an account and workspace reserves a UUID in private storage, then
posts it as `clientThreadId` to `/api/chat-threads` with the title `Cardputer Zero`.
A retry reuses that reservation. Later launches load the saved thread's metadata.
Only a 404 or 410 on an established thread allows the client to create a replacement.
The binding includes the API origin, user and organization.

## History and reader

The client fetches `/api/chat-threads/<id>/event-snapshot` and reads the signed
NDJSON archive without sending OAuth or client headers to storage. A canonical
`CHAT_EVENT_SNAPSHOT_NOT_FOUND` starts history at sequence zero. The client follows
`event-rows` using the sequence and event ID from the same cursor. It validates the
thread, strict ordering and final cursor before publishing history. An expired cursor
permits one rebuild. Limits are 8,192 events per reconstruction, 200 recent rows for
presentation and 32 KiB of text per message.

Assistant `output.message.payload.content` is Markdown, including literal code and
JSON. The reader formats `createdAt` in the device's local time zone when available.
Approval links and unsupported actions direct the reader to Okou Web. Input
metadata and tool events are omitted from the reader.

An input carrying `revokesEventId` replaces the referenced row and retains the
original input identity for playback and acknowledgement. Independent sends of the
same clip remain distinct. Run completion, cancellation and failure apply only to
rows with the matching run ID.

## Voice messages

Holding Enter captures 16 kHz mono PCM16 through the configured ALSA device
(default `pulse`) for up to ten minutes. Releasing Enter after a hold of at least
350 ms sends the recording. Shorter taps discard it. The waveform uses the captured
sample peaks.

1. Prepare a private WAV upload with `POST /api/uploads/prepare/private`.
2. PUT the WAV to the signed storage URL with the supplied upload headers and
   complete the upload with `POST /api/uploads/complete`.
3. Read the fixed thread's metadata to preserve its computer host and browser settings.
   Submit `POST /api/chat/events` with the thread ID, a version-1 user message
   containing a voice part with `clips: [{fileId}]` and an `additional_info` part describing
   the handheld reader's screen capacity, Markdown support and link limitations. Only the
   voice part appears in the conversation. The request uses `hasTextContent: false` and
   the protocol prompt `(see message parts)`.

Before submission the client durably reserves both event UUIDs in an account- and
thread-scoped file. A matching successful response clears the reservation. Definite
4xx rejections also clear it, except 408 and 409. An ambiguous response keeps the
reservation across restarts. A complete, validated history read releases it after
finding the matching input ID or its promotion.

An acknowledged recording is playable immediately. Space selects clips associated
with the current reply or a newly sent recording. Playback obtains a signed URL from
`GET /api/voice/clips/<fileId>/playback`, spools the WAV to disk and plays it through
PulseAudio. Pause and resume keep the playback position.

## Realtime

The client exchanges the signed TokenRequest from `POST /api/realtime/token` with
Ably. Its WebSocket uses protocol v6 and attaches in subscribe mode to
`user-org:<user>:<org>`. Attaching, losing channel continuity, or receiving
`chatThreadMessageCreated:<id>` or `chatThreadDetailChanged:<id>` for the fixed
thread schedules a history refresh. The callback carries no message content.

The worker renews credentials before expiry or on an AUTH challenge, and reconnects
after transient failures with bounded exponential backoff. A reconnect supplies
the connection key and channel serial when available and catches up after attach.
