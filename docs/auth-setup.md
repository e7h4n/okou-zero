# Authentication and private configuration

Okou Zero is an independent MIT C++17 client. Its transport/authentication implementation is newly authored; Okou source was consulted only for wire protocols. It never impersonates the iOS client and never embeds a Clerk secret.

## Operator setup required before login

Official CI packages bundle the public OAuth client ID from the GitHub `staging` Environment variable `OKOU_ZERO_OAUTH_CLIENT_ID`. This Environment is only the configuration source: there is one standard package, not separate staging/production variants. An Okou administrator must provision the application in the matching Clerk instance:

1. Enable **Public** and **Device authorization grant** for that application (`public: true`, `device_authorization_grant_enabled: true` in Clerk's administrative API). Enable refresh-token support / `offline_access` for the grant. The native client uses token endpoint authentication `none`, sends only `client_id`, and does not store or send a client secret. Device grant uses neither PKCE nor a redirect URI.
2. Enable Organizations and allow `user:org:read`. Clerk's device verification page displays an Organization selector when this scope is requested. The user selects the workspace in that browser; the resulting access token must contain its `org_id`.
3. Configure JWT access tokens, resource audience support and the resource accepted by the Okou API (normally `https://api.okou.ai/mcp`). The API source verifies a signed OAuth access token (`at+jwt`), its issuer, resource audience, `sub`, `org_id`, `client_id`, expiry and granted scopes. An opaque token or an unscoped user token is not a substitute.
4. Allow the actual backend scopes used by this client. The reference API requires **`user:org:read okou:chat:read`**, plus `okou:chat:send` to send. `openid profile offline_access` are useful for the device/refresh flow. The reference MCP full grant is `openid email profile user:org:read okou:chat:read okou:chat:send okou:chat:manage okou:run:cancel offline_access`. This client does not need manage/cancel merely to read and send. Project and voice operations additionally need the separately coordinated backend scopes `okou:project:read` and `okou:voice:transcribe`; those are NOT advertised by the original reference MCP metadata. They must be installed and allowed server-side, not just added to a local string. Check the integrated backend and `config.example.json` for the final supported grant.
5. Supply the provisioned public client ID below. An issuer may be configured explicitly; otherwise the client reads `<api_origin>/.well-known/oauth-protected-resource/mcp` and uses the first advertised authorization server. It then reads the issuer's OIDC configuration, falling back on a 404 to RFC 8414 OAuth authorization-server metadata. Device and token URLs come from metadata, never guessed provider paths. Missing grant/endpoint support is a setup error.

**Registration feasibility:** Clerk officially supports manually provisioned public device applications. DCR is optional and separately enabled by the administrator; advertising a registration endpoint does not prove that a particular dynamically registered application can use the device grant. CIMD is another onboarding option but is not implemented or assumed for device grant here. This client performs **no registration**, never invokes the Clerk Backend API, and never acquires an administrative secret. Provider documentation confirms the feature, not that Okou's live Clerk instance/application has already enabled it. That last check and app provisioning remain operator responsibilities.

**Workspace switching:** reauthorize with a new device code and select the other Organization on Clerk's verification page, then replace the token set and reconnect/reload the client in the owning application. No documented device `organization_id` request override was established, so none is sent. Editing the persisted `org_id` or adding a workspace header cannot grant access; refresh does not switch organizations. The shared OAuth API has no organization-list/switch operation.

Sources checked 2026-11-17:
- [Clerk Device Authorization Grant](https://clerk.com/docs/guides/configure/auth-strategies/oauth/device-authorization-grant)
- [How Clerk implements OAuth](https://clerk.com/docs/guides/configure/auth-strategies/oauth/how-clerk-implements-oauth)
- Okou protocol references: `turbo/apps/api/src/lib/mcp-server-config.ts`, `signals/routes/mcp-server.ts`, `signals/external/clerk.ts`.

## Bundled defaults

Starting with 0.2.5, the standard package targets the production API at `https://api.okou.ai`. The resource is this origin plus `/mcp`; issuer is discovered from its OAuth resource metadata. These defaults live in code, so no additional GitHub API/issuer/resource Variables are needed. Only `OKOU_ZERO_OAUTH_CLIENT_ID` is injected by CI as a CMake string. It is public, not a secret. Main/tag builds fail closed if that Variable is missing; local/fork offline builds may leave it empty and show the setup screen.

Configure locally with `cmake -S . -B build -DOKOU_ZERO_OAUTH_CLIENT_ID=YOUR_PUBLIC_CLIENT_ID`. Runtime environment variables still override user configuration, which overrides bundled defaults. Existing user configuration is preserved, not silently rewritten. Existing grants remain bound to API/issuer/resource/client ID and require fresh authorization when those change. There is no special test storage directory, test UI label, or alternate updater channel; normal signed stable updates remain enabled.

The bundled public client must be provisioned in production Clerk (`https://clerk.okou.ai`) and allow the requested Project/Voice scopes. No Preview bypass credential is embedded or needed. The former Preview grant is not reused: changing the origin/resource/client binding requires fresh device authorization. Existing explicit user configuration and environment overrides remain authoritative; operators must deliberately update any old Preview override. Production metadata availability and configured scope strings are not proof of end-to-end login or voice acceptance.

## Configuration

Default paths:

- Configuration: `$XDG_CONFIG_HOME/okou-zero/config.json`, or `$HOME/.config/okou-zero/config.json` if XDG_CONFIG_HOME is unset or relative.
- Credentials: `<data_dir>/tokens.json`; default `data_dir` is `$XDG_DATA_HOME/okou-zero`, or `$HOME/.local/share/okou-zero` if XDG_DATA_HOME is unset or relative.
- `OKOU_ZERO_CONFIG` chooses an alternate configuration path for load/save. `load_config(path)` can explicitly choose a read path; `save_config` uses the environment/default path, because its shared signature has no path argument.

Create a user-only directory and file; do not use the shipped example as live writable configuration:

```sh
install -d -m 700 "$HOME/.config/okou-zero"
# Copy the package's config.example.json here, then chmod 600 config.json.
```

Minimal JSON (replace the public ID; no secret):

```json
{
  "oauth_client_id": "YOUR_APPROVED_PUBLIC_CLIENT_ID",
  "scopes": "openid profile offline_access user:org:read okou:chat:read okou:chat:send okou:project:read okou:voice:transcribe"
}
```

Environment overrides JSON, including explicitly empty values:

| JSON field | Environment override |
|---|---|
| `api_origin` | `OKOU_ZERO_API_ORIGIN` |
| `oauth_issuer` | `OKOU_ZERO_OAUTH_ISSUER` |
| `oauth_client_id` | `OKOU_ZERO_OAUTH_CLIENT_ID` |
| `oauth_resource` | `OKOU_ZERO_OAUTH_RESOURCE` |
| `scopes` | `OKOU_ZERO_SCOPES` |
| `data_dir` | `OKOU_ZERO_DATA_DIR` (absolute path) |
| `keyboard` | `OKOU_ZERO_KEYBOARD` |
| `framebuffer` | `OKOU_ZERO_FRAMEBUFFER` |
| `capture_device` | `OKOU_ZERO_CAPTURE_DEVICE` |

Config and token files must be regular files owned by the current effective user, with no group/other permissions and only one hard link. The containing directory must be user-owned with no group/other permissions (use 0700). Symlink traversal, including directory symlinks, is rejected. Files are bounded to 1 MiB. Saves create a 0600 temporary file in the destination directory, write/fsync, atomically rename, then fsync the directory. This provides crash-safe local generation replacement, not encryption at rest or protection from root/the same user; use full-disk encryption for a lost device. No interprocess token-refresh locking is supplied: run one client per credential directory.

### Credential grant binding

Each token record contains a versioned `grant_binding` for `api_origin`, `oauth_resource`, the **configured** `oauth_issuer`, and the public `client_id`. API origin comparison removes trailing slashes only; resource, client ID and configured issuer are exact strings. An empty issuer means resource discovery and is distinct from an explicit issuer, even if discovery resolves to that same URL. Validated metadata's issuer is saved separately as `resolved_oauth_issuer` provenance after discovery; it does not replace the configured binding.

Loading compares this binding locally **before** exposing access or refresh tokens, with no discovery or other network request. A different origin, resource, issuer or client ID using the same `data_dir` receives an empty token set and must authorize again. Old unbound records and incomplete/unknown bindings also require a fresh device grant: they are never automatically migrated or rebound. A rejected record is left untouched until a new login replaces it or logout removes it. Refresh/save preserves the configured binding. Changing scopes alone is not a grant-binding change; reauthorize when requesting additional permissions.

Use a separate `data_dir` per configuration if you need to retain multiple grants; the directory still holds only one token generation. This prevents accidental cross-configuration reuse, not malicious changes to both configuration and credential files by the same OS user. An unchanged discovery configuration is not revalidated against live metadata at startup; metadata is fetched when beginning or refreshing a grant.

## Device and token lifecycle

`begin()` returns a private `device_code`, public user code and verification URI(s). Show only the user code and verification links, never the device code or tokens. Main owns cancellation and scheduling: wait at least `interval` before **each** poll, increase it by at least five seconds on `slow_down`, stop at expiry, denial or cancellation, and avoid overlapping polls. `poll()` returns `pending`, `slow_down`, `authorized`, `denied`, or `expired`; other provider errors become sanitized errors without `error_description`.

After authorization or refresh, main saves the returned Tokens. Refresh rotates the refresh token when provided, retaining the previous one when omitted. Do not automatically retry an interrupted refresh: a refresh token may have rotated even when the response was lost. A failed token request writes a token-free reauthorization barrier before returning; subsequent refresh attempts and process restarts cannot reuse that generation. A fresh device authorization replaces the barrier. Discovery failures before a token request do not invalidate a grant. Access JWT payloads are decoded **without verification** solely for display, local expiry scheduling and realtime channel addressing. The API remains the authority. A user/org-bound token is required locally; load re-derives claims from the access token rather than trusting editable persisted user/org fields. Persisted expiry can only shorten the JWT expiry.

Logout removes/fsyncs local credentials **before** best-effort revocation. Cancellation, discovery errors, network failure or a provider rejection cannot prevent the local clear. Revoking the refresh token is preferred; the process must also stop realtime and discard its in-memory token set in main. Deleting an SSD file is not guaranteed physical secure erasure, and failed remote revocation can leave a previously issued access token valid until expiry.

## HTTP contract and security

- HTTPS with certificate and hostname verification and TLS 1.2 minimum. Redirect following is disabled for all requests; bearer tokens cannot be forwarded to a redirect target. Signed snapshot requests omit bearer and client identity headers.
- Bearer API requests use `X-Client-Type: OkouZero`, `X-Client-Version` matching the compiled application version, and a fresh random `X-Client-Request-Id`; no iOS identity or invented organization header. Multipart values are in-memory bytes, not local filenames to read.
- Connect timeout 10 seconds, total timeout 60 seconds, low-speed timeout 20 seconds, cancellation via libcurl progress callback. Gzip is transparently decoded. Maximum decoded response body 16 MiB and headers 64 KiB.
- HTTP 2xx/3xx/4xx return `HttpResponse`; **every caller must check status** before interpreting a successful API response. OAuth reads the standard 400 error field. Network/cancellation/response-bound errors and 5xx throw sanitized `HttpError`. Non-read requests interrupted after transport starts, or receiving 5xx, are conservatively `uncertain=true`; preflight cancellation is not uncertain. No request is automatically replayed.
- Errors contain only fixed descriptions/status, never request URL, body, token, provider error description or raw response. Debug/verbose curl logging is not enabled.

### Sanitized failure diagnostics

The client writes bounded, single-line structured failure records to **stderr**. When launched by the standard launcher service, these use the launcher's existing journal stream; the client creates no log files, telemetry service, network request or new retention policy. For example:

```sh
journalctl --user -u APPLaunch.service --since '10 minutes ago' --no-pager
```

Records identify the failed local operation, HTTP status and numeric libcurl error code when available, send uncertainty, and an allowlisted OAuth protocol error. Unknown provider codes and arbitrary exception text become fixed classifications rather than being copied into logs. HTTP status zero means no usable HTTP status was available, not that the provider returned a status of zero. A libcurl code is a transport diagnostic, not proof of a specific server-side cause.

Never enable verbose curl or request/response dumps to investigate login. These diagnostics exclude URLs and query strings, request/response bodies, headers, provider descriptions, tokens, device/user codes, verification links, client/user/organization identifiers, messages, drafts and audio. Normal authorization-pending and slow-down polling stays quiet. Logging does not change authorization, polling intervals, validation, refresh barriers or uncertain-send handling, and never retries an operation. A stage-specific record and an application-boundary record can describe the same exception; two such records do not imply two network requests.

Older installed versions cannot recover details they never recorded. After an approved diagnostic build is installed, reproduce the failure only with explicit operator approval; adding diagnostics or opening a PR does not authorize another login, device installation or service change.

### Offline fixtures only

`OKOU_ZERO_TEST_ALLOW_LOOPBACK_HTTP=1` explicitly allows HTTP **only** to numeric `127.0.0.1` / `::1` fixtures (not hostname `localhost`, arbitrary hosts or redirects). `OKOU_ZERO_TEST_CA_FILE` supplies a test CA **only** for numeric-loopback HTTPS; certificate and hostname verification stay enabled. Neither setting bypasses TLS validation for public services. Keep them unset in production.

`auth_test` and `http_test` each have their own `main` and throwing checks active with `NDEBUG`. They use only local sockets; HTTP tests generate a temporary self-signed loopback TLS certificate with the `openssl` test dependency. No public network, real account or client registration is used.
