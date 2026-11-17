# Authentication and private configuration

Okou Zero is an independent MIT C++17 client. Its transport/authentication implementation is newly authored; Okou source was consulted only for wire protocols. It never impersonates the iOS client and never embeds a Clerk secret.

## Operator setup required before login

There is deliberately **no bundled OAuth client ID**. An Okou administrator must provision an OAuth application in the correct Clerk instance:

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
  "scopes": "openid profile offline_access user:org:read okou:chat:read okou:chat:send"
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

## Device and token lifecycle

`begin()` returns a private `device_code`, public user code and verification URI(s). Show only the user code and verification links, never the device code or tokens. Main owns cancellation and scheduling: wait at least `interval` before **each** poll, increase it by at least five seconds on `slow_down`, stop at expiry, denial or cancellation, and avoid overlapping polls. `poll()` returns `pending`, `slow_down`, `authorized`, `denied`, or `expired`; other provider errors become sanitized errors without `error_description`.

After authorization or refresh, main saves the returned Tokens. Refresh rotates the refresh token when provided, retaining the previous one when omitted. Do not automatically retry an interrupted refresh: a refresh token may have rotated even when the response was lost. Surface reauthorization if recovery is impossible. Access JWT payloads are decoded **without verification** solely for display, local expiry scheduling and realtime channel addressing. The API remains the authority. A user/org-bound token is required locally; load re-derives claims from the access token rather than trusting editable persisted user/org fields. Persisted expiry can only shorten the JWT expiry.

Logout removes/fsyncs local credentials **before** best-effort revocation. Cancellation, discovery errors, network failure or a provider rejection cannot prevent the local clear. Revoking the refresh token is preferred; the process must also stop realtime and discard its in-memory token set in main. Deleting an SSD file is not guaranteed physical secure erasure, and failed remote revocation can leave a previously issued access token valid until expiry.

## HTTP contract and security

- HTTPS with certificate and hostname verification and TLS 1.2 minimum. Redirect following is disabled for all requests; bearer tokens cannot be forwarded to a redirect target. Signed snapshot requests omit bearer and client identity headers.
- Bearer API requests use `X-Client-Type: OkouZero`, `X-Client-Version: 0.1.0`, and a fresh random `X-Client-Request-Id`; no iOS identity or invented organization header. Multipart values are in-memory bytes, not local filenames to read.
- Connect timeout 10 seconds, total timeout 60 seconds, low-speed timeout 20 seconds, cancellation via libcurl progress callback. Gzip is transparently decoded. Maximum decoded response body 16 MiB and headers 64 KiB.
- HTTP 2xx/3xx/4xx return `HttpResponse`; **every caller must check status** before interpreting a successful API response. OAuth reads the standard 400 error field. Network/cancellation/response-bound errors and 5xx throw sanitized `HttpError`. Non-read requests interrupted after transport starts, or receiving 5xx, are conservatively `uncertain=true`; preflight cancellation is not uncertain. No request is automatically replayed.
- Errors contain only fixed descriptions/status, never request URL, body, token, provider error description or raw response. Debug/verbose curl logging is not enabled.

### Offline fixtures only

`OKOU_ZERO_TEST_ALLOW_LOOPBACK_HTTP=1` explicitly allows HTTP **only** to numeric `127.0.0.1` / `::1` fixtures (not hostname `localhost`, arbitrary hosts or redirects). `OKOU_ZERO_TEST_CA_FILE` supplies a test CA **only** for numeric-loopback HTTPS; certificate and hostname verification stay enabled. Neither setting bypasses TLS validation for public services. Keep them unset in production.

`auth_test` and `http_test` each have their own `main` and throwing checks active with `NDEBUG`. They use only local sockets; HTTP tests generate a temporary self-signed loopback TLS certificate with the `openssl` test dependency. No public network, real account or client registration is used.
