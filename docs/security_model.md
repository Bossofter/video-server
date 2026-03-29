# Security Model

This repo is hardened for local development by default, not for open Internet deployment.

## Default Safe Behavior

Default `WebRtcVideoServerConfig` behavior is intentionally conservative:

- HTTP binds to `127.0.0.1`
- debug API is disabled
- runtime config and signaling stay local by default
- CORS is not wildcard by default

This means a default server instance is aimed at same-machine development first.

## Shared-Key Auth

The server can protect sensitive routes with a shared key.

Configuration:

```cpp
video_server::WebRtcVideoServerConfig cfg;
cfg.enable_shared_key_auth = true;
cfg.shared_key = "dev-secret";
```

Accepted headers:

- `Authorization: Bearer <key>`
- `X-Video-Server-Key: <key>`

Example:

```bash
curl \
  -H 'Authorization: Bearer dev-secret' \
  http://127.0.0.1:8080/api/video/streams/alpha/output
```

The NiceGUI harness supports this directly with `--shared-key`.

## Allowlist Behavior

`ip_allowlist` limits remote access to approved IPs or CIDR ranges.

Practical use:

- keep loopback access open for local development
- allow a trusted LAN segment for remote smoke testing

The allowlist is a route guard, not a user identity system.

## LAN-Only Mode

The synthetic smoke server exposes a practical LAN testing mode through `--lan-only`.

That mode enables:

- permissive CORS
- remote access to signaling
- remote access to runtime config
- remote access to debug routes

Use it only on a trusted LAN.

Example:

```bash
python examples/nicegui_smoke/app.py \
  --start-server \
  --multi-stream-demo \
  --auto-connect \
  --lan-only
```

## What Is Not Implemented

This repo does not provide:

- user accounts
- sessions/cookies
- OAuth/OIDC
- per-user authorization
- production-grade Internet edge security

Treat the shared key and allowlist as developer-oriented protection for local and trusted-LAN use.

## Practical Guidance

- use default loopback binds unless you are actively doing LAN tests
- turn on shared-key auth when multiple people or machines can reach the server
- only enable debug and unsafe public routes when you need them
- do not treat `--lan-only` as a production deployment profile
