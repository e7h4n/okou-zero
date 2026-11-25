# Authentication and configuration

## Device sign-in

Official packages include a public Clerk device client. Press Enter on the login screen,
open the displayed verification URL in a browser, enter the code and choose a workspace.
To change workspace, use **Ctrl+W → Switch workspace** and authorize a new grant.

For a custom build, enable the device authorization grant and refresh tokens on a public
Clerk OAuth application. Set the public client ID at build time with
`-DOKOU_ZERO_OAUTH_CLIENT_ID=YOUR_PUBLIC_CLIENT_ID` or enter it on the handheld's setup
screen. The client requests `openid profile offline_access user:org:read okou:chat:read
okou:chat:send`. Reading history, realtime notifications and playback use chat read access.
Creating the fixed thread, uploading private audio and sending its clip use chat send access.

## Bundled defaults

CI reads the public `OKOU_ZERO_OAUTH_CLIENT_ID` variable from the repository's `staging`
GitHub Environment. The package connects to `https://api.okou.ai` with resource
`https://api.okou.ai/mcp`. By default the client discovers the issuer through
`/.well-known/oauth-protected-resource/mcp`. It reads the issuer's OpenID configuration,
or its RFC 8414 metadata if the OpenID document returns 404. Device authorization,
polling, refresh and revocation use the endpoints advertised by that issuer.

## Local settings

Environment variables override the JSON configuration, which overrides bundled defaults.
The configuration defaults to `$XDG_CONFIG_HOME/okou-zero/config.json` or
`$HOME/.config/okou-zero/config.json`. Select another path with `--config PATH` or
`OKOU_ZERO_CONFIG`. Account data defaults to `$XDG_DATA_HOME/okou-zero` or
`$HOME/.local/share/okou-zero`. Use absolute XDG paths.

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

Use a user-owned 0700 directory and a regular 0600 file. Symlinks and hard-linked
private files are rejected. The package ships a [configuration example](../config/config.example.json).

## Credentials

The grant in `<data_dir>/tokens.json` is bound to the configured API origin, resource,
issuer and public client ID. Changing any binding requires a new device authorization.
Use a separate data directory for each grant and run one client per directory.
The client takes the user and workspace from the access token. The API enforces its
signature, audience, membership and scopes.

Refresh replaces the stored token generation atomically. If a refresh request has
an uncertain outcome, sign in again to obtain a fresh grant. Logout clears the local
credentials before attempting remote revocation.

## Sanitized failure diagnostics

Client failures go to stderr. On a device using the standard launcher service, inspect
the existing journal:

```sh
journalctl --user -u APPLaunch.service --since '10 minutes ago' --no-pager
```

The record includes the operation, HTTP status, numeric libcurl code, send uncertainty
and an allowlisted OAuth error. Request URLs, bodies, tokens and recordings stay out of
the log.

## Local test transport

Loopback fixtures use `OKOU_ZERO_TEST_ALLOW_LOOPBACK_HTTP=1` for numeric loopback
addresses and `OKOU_ZERO_TEST_CA_FILE` for a local HTTPS CA. Leave both unset on the
handheld. Production transport verifies TLS certificates and hostnames.
