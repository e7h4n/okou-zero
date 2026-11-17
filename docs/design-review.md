# V1 design and compatibility review

Status: **proposed, awaiting owner decisions**. Reviewed 2026-11-17 against Okou commit `30ac1c7c85be3d065b9330d90b86c14c3c5d6b0a`.

## Reference boundaries

The iOS client is the behavioral reference for chat synchronization, indicators, conversation state and voice input. It uses organization-scoped Clerk **session tokens**. The requested device login uses Clerk **OAuth access tokens**, which are not interchangeable with session tokens or Okou CLI tokens.

The main Okou repository is BSL-1.1. Public visibility does not grant permission to copy its implementation under an unrelated license. Decide licensing before application source is introduced here.

## Authentication compatibility: source-level evidence

| Capability | Current source evidence | Consequence |
| --- | --- | --- |
| Clerk device login | Clerk documents a public client, discovery, device authorization and refresh tokens | Register/configure an eligible OAuth application. Never ship a client secret. |
| OAuth validation | `turbo/apps/api/src/signals/auth/auth-context.ts` and `signals/external/clerk.ts` | OAuth requires an explicit per-operation `oauthScope`, a signed organization identity, membership and the configured audience. |
| Thread/event reads | `signals/routes/chat-threads.ts` | Selected reads already declare `okou:chat:read`; this is not proof that all iOS sync operations accept OAuth. |
| Chat submission | `signals/routes/chat-events.ts` | OAuth scopes exist for selected event operations; request bodies and operation-specific authorization still apply. |
| Projects | `signals/routes/projects.ts` | Routes do not currently declare `oauthScope`; raw OAuth access tokens are not admitted through this path. |
| Ably authentication | `signals/routes/realtime-token.ts` | Calls `requiredAuthContext$` without an OAuth scope. Needs an explicit, least-privilege OAuth policy. |
| Voice quota | `signals/routes/voice-io-quota.ts` | No OAuth scope currently declared. |
| Voice transcription | `signals/routes/voice-io-transcribe.ts` | Explicit `accept: ["session"]`. A device OAuth access token cannot simply replace the iOS token. |

This is a review of a source commit, **not** a live production authorization test. No production token, recording, user message or billing event was created to test these endpoints.

Do not solve the mismatch by weakening token validation, accepting an ID token as an access token, bundling a backend secret, or silently substituting the CLI token flow. A minimal companion change to the main API and a configured public OAuth application are expected prerequisites.

## iOS behavior to preserve

Reference paths below are under `ios/` in the pinned Okou commit:

- `Packages/ChatData/Sources/ChatData/ChatSync.swift`: thread-list snapshots and incremental events have a cursor separate from each conversation's event snapshot/tail. Handle expired baselines by rebuilding; persist state and cursor atomically. Signed snapshot downloads must omit the bearer token.
- `Okou/Services/RealtimeService.swift`: `/api/realtime/token` returns an Ably **TokenRequest**, not an assumed token string. Attach `user-org:<userID>:<workspaceID>` and `user:<userID>`; recover authoritative state on a gap.
- `Okou/App/WorkspaceReadBootstrap.swift`: subscribe before the authoritative indicator read when possible, with a bounded HTTP fallback and a confirming read if attachment happened later.
- `Okou/Domain/ChatInvalidation.swift`: `threadListChanged` refreshes list metadata, while `chatIndicatorsChanged` refreshes indicators. Do not reintroduce the historical list-change indicator fallback.
- `Packages/ChatData/Sources/ChatData/ChatCommands.swift`: preserve stable client event identity and treat ambiguous sends as uncertain, not automatically retryable.
- `Packages/ChatData/Sources/ChatData/VoiceInputClient.swift` and `VoiceDraftSession.swift`: 16 kHz mono PCM16, 15-second transcription windows with 2-second overlap, quota check, final segment, polish, then editable draft insertion. Ending dictation is not submitting a message. Cardputer recording persistence/limits still need a deliberate implementation choice.

These are protocol/behavior observations, not copied application source. Do not send `X-Client-Type: iOS` from this client or invent an organization header as a substitute for an organization-bound token.

## Proposed user experience

- Login: show a short user code and verification URL; optionally show a QR code. Honor provider polling intervals, slow-down responses, expiration and denial.
- The OAuth grant must bind an explicit workspace; a grant never authorizes access to another workspace merely because an ID was supplied by the client.
- Navigation: workspace/project selection, thread list with running/unread state, conversation history, and composer.
- Typing: keyboard-driven focus, UTF-8 text, explicit submit, and draft preservation across transient failures.
- Voice proposal: deliberate recording start/stop, server-side transcription into an editable draft, explicit submit. No always-on microphone, automatic send or spoken response in this proposal.
- Rich approval/action cards: safely label unsupported actions and direct the user to the web client; do not execute approvals implicitly. Exact V1 presentation remains to be decided.

## Proposed architecture and safety properties

- Keep the UI thread nonblocking. HTTP, Ably and recording operate under cancellable lifecycle ownership.
- Use bounded transcript/list windows and caches; a device must not need to retain every historical event in memory.
- Preserve authoritative snapshot/cursor semantics from the canonical APIs. Realtime notifications are not assumed to replace durable catch-up.
- Attach realtime subscriptions before the corresponding authoritative read when required by the canonical synchronization protocol. Reconcile after reconnects and gaps.
- Send messages with the server-supported stable operation identity. Do not blindly resubmit an uncertain write or display an unsent message as delivered.
- Verify TLS, validate downloaded snapshot URLs and never forward bearer credentials to an unrelated signed object-storage URL.
- Store refresh credentials in user-only files (directory 0700, files 0600, atomic replacement). This is not protection against root or theft of an unencrypted SD card.
- Logout cancels tasks, revokes credentials where supported, clears local credentials and removes account-bound caches. Logs never include tokens, device codes, transcript text or raw audio.
- No additional inbound service is needed on the device.

## Proposed CI and acceptance contract

Public pull requests run without repository secrets or access to real Okou accounts.

| Layer | Required checks |
| --- | --- |
| Static checks | Formatting, warnings, dependency/license review and secret scanning |
| Unit tests | Device-code state machine, refresh rotation, cancellation, state reduction, pagination, indicator precedence, draft lifecycle and bounded history |
| Protocol integration | Local fake Clerk, HTTP and Ably servers; pending/denied/expired login, token expiry, connection gaps, catch-up, malformed events, expired snapshots and uncertain sends |
| Voice integration | Synthetic non-private WAV fixtures; recording cancellation, empty audio, quota exhaustion, transcription errors and draft insertion |
| UI tests | Headless desktop simulator at 320×170; keyboard navigation, long titles, multibyte text, disconnected/auth-error states and deterministic screenshots |
| Packaging | ARM64 build, `.deb` metadata/dependency validation, clean install/uninstall smoke test in an ARM64 CI environment |
| Security | Fork PRs have read-only permissions and no production credentials. Never use `pull_request_target` to execute contributor code with privileged tokens. |
| Release | Versioned package, checksums and build provenance; release publication policy to be confirmed |
| Device | Install the CI-built package; verify physical framebuffer, actual keyboard, microphone, audio device selection, memory and reconnection. Do not compile on the Cardputer. |

Mock integration tests do not replace one real Clerk login, one real Ably subscription or a physical microphone acceptance test. Real message submission and paid voice calls require a deliberate acceptance procedure, not public CI.

## Decisions required before implementation

1. May this project include a companion PR in `okou-ai/okou` to add narrowly scoped OAuth admission to the necessary API operations? How should its review/release be handed off? No automatic merge is authorized.
2. Does “voice communication” mean recording -> transcription -> editable draft -> explicit send for V1, or is spoken output/live voice also required?
3. What license should the new repository use? A standalone MIT implementation without copied BSL code is one option; retaining BSL when reusing upstream implementation is another.
4. Workspace UX and unsupported action-card UX need explicit agreement before implementing those branches. Recommended V1: bind one explicit workspace per grant, allow reauthorization to switch, and open unsupported approval flows in the web client.

No client application code, GitHub Actions workflow, backend patch or device deployment has been implemented at this stage.
