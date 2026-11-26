# Cardputer Zero client protocol

## Network endpoints

All authenticated Okou chat operations use `POST https://api.okou.ai/mcp`. This
is the only production endpoint that receives an Okou bearer authorization header.
The device uploads WAVs to an HTTPS Cloudflare R2 endpoint returned by `prepare_voice_upload`.
Snapshot downloads use signed GET URLs supplied by MCP. File transfers
carry no Okou access token and do not follow redirects.

Device sign-in, token renewal and revocation use the [OAuth endpoints](auth-setup.md).
Realtime delivery uses Ably. The standalone package checks GitHub for
[signed updates](updates.md). The Store edition receives updates through the launcher.

## Fixed conversation

The first launch for an account and workspace reserves a UUID in private storage, then
calls the MCP tool `create_chat_thread` with that `clientThreadId` with the title `Cardputer Zero`.
A retry reuses that reservation. Later launches check the saved thread with `get_chat_messages`.
Only the tool's explicit not-found result permits replacing an established thread.
The binding includes the API origin, user and organization.

## History and reader

The first `get_chat_messages` call returns a snapshot pointer and the first page
of event rows together. The client reads a signed NDJSON archive without OAuth
or client headers, then applies the inline rows. While more pages are available it
continues with the matching sequence and event ID. It validates thread identity,
strict ordering and the returned cursor before committing its in-memory history.
After an Ably notification or reattach, it resumes from that cursor. An expired
cursor permits one rebuild. Limits are 8,192 events per reconstruction, 200 recent rows for
presentation and 32 KiB of text per message.

Assistant `output.message.payload.content` is Markdown, including literal code and
JSON. The reader formats `createdAt` in the device's local time zone when available.
Approval links and unsupported actions direct the reader to Okou Web. Input
metadata and tool events are omitted from the reader.

An input carrying `revokesEventId` replaces the referenced row and retains the
original input identity for acknowledgement. Independent sends of the
same clip remain distinct. Run completion, cancellation and failure apply only to
rows with the matching run ID.

## Voice messages

Tap Enter to capture 16 kHz mono PCM16 through the configured ALSA device
(default `pulse`) for up to ten minutes. Tap Enter again to send, or press Esc to
discard. The waveform uses the captured sample peaks.

1. Call `prepare_voice_upload` with the WAV filename and byte count.
2. PUT the audio to the signed URL with its upload headers and call `complete_voice_upload`.
3. Call `send_chat_message` with the fixed thread ID, a durable `clientEventId`
   and a version-1 `userMessage`. Its voice part carries the clip file ID and its
   additional-info part describes the handheld reader.

Before submission the client persists the event ID in the existing private
account- and thread-scoped ledger. A matching successful response clears the reservation. Definite
4xx rejections also clear it, except 408 and 409. An ambiguous response keeps the
reservation across restarts. A complete, validated history read releases it after
finding the matching input ID or its promotion.

## Realtime

The client exchanges the signed TokenRequest from `get_chat_realtime_token` with
Ably. Its WebSocket uses protocol v6 and attaches in subscribe mode to
`user-org:<user>:<org>`. Attaching, losing channel continuity, or receiving
`chatThreadMessageCreated:<id>` or `chatThreadDetailChanged:<id>` for the fixed
thread schedules a history refresh. The callback carries no message content.

The worker renews credentials before expiry or on an AUTH challenge, and reconnects
after transient failures with bounded exponential backoff. A reconnect supplies
the connection key and channel serial when available and catches up after attach.
