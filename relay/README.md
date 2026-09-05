# FastSMRW push relay

Bridges Mastodon **Web Push** to Apple **APNs** so the iOS app can receive
notifications while it isn't running. It is a blind forwarder: Mastodon posts an
*encrypted* Web Push body to a per-device endpoint here, and the relay forwards
that still-encrypted blob to APNs as a `mutable-content` alert. The app's
Notification Service Extension decrypts it on-device. **The relay never holds the
decryption keys and never sees notification contents.**

Standard-library Go, no dependencies.

## Build

```sh
cd relay
GOOS=linux GOARCH=amd64 CGO_ENABLED=0 go build -trimpath -o dist/fastsm-push-relay .
```

Produces a static linux/amd64 binary (the brynify VM is Debian 13 x86_64).

## HTTP API

- `POST /register` — body `{"device_token","environment","endpoint_id?"}`;
  returns `{"endpoint":"<public_base>/push/<id>"}`. `environment` is
  `sandbox` (dev-signed builds) or `production` (TestFlight/App Store).
  Optionally protected by a Bearer `RELAY_REGISTER_TOKEN`.
- `POST /push/<id>` — Mastodon posts the encrypted Web Push body here.
- `GET /healthz` — liveness.

## Install on the VM (mew@brynify.me)

The binary + unit + env template are staged in `~/fastsm-push-relay-staging/`.
These steps need sudo.

```sh
sudo install -m 0755 ~/fastsm-push-relay-staging/fastsm-push-relay /usr/local/bin/
sudo install -m 0644 ~/fastsm-push-relay-staging/fastsm-push-relay.service /etc/systemd/system/
sudo mkdir -p /etc/fastsm-push-relay
sudo install -m 0640 ~/fastsm-push-relay-staging/relay.env.example /etc/fastsm-push-relay/relay.env
# Put the APNs key here (mode 600) and edit relay.env to match:
sudo install -m 0600 /path/to/AuthKey_DA36MM9RZR.p8 /etc/fastsm-push-relay/
sudo $EDITOR /etc/fastsm-push-relay/relay.env      # set RELAY_PUBLIC_BASE etc.
sudo systemctl daemon-reload
sudo systemctl enable --now fastsm-push-relay
curl -s http://127.0.0.1:8787/healthz              # -> {"status":"ok"}
```

## Expose it over HTTPS (fits the existing SNI router)

The front host's `:443` is a two-tier SNI stream router
(`/etc/nginx/streams-router.conf`) that adds PROXY protocol. To add
`push.brynify.me` terminating on this host:

1. **DNS** (you): `push.brynify.me  A  38.143.59.76`.

2. **TLS cert** for `push.brynify.me` (DNS-01 is simplest given :443/:80 are
   already routed):
   ```sh
   sudo certbot certonly --preferred-challenges dns -d push.brynify.me
   ```

3. **Tier-1 map** — add one line to the `$tier1` map in
   `/etc/nginx/streams-router.conf` (leave the existing entries untouched):
   ```nginx
   push.brynify.me  127.0.0.1:8500;
   ```

4. **TLS-terminating vhost** at `127.0.0.1:8500` — new file
   `/etc/nginx/sites-available/push.conf`, symlinked into `sites-enabled/`:
   ```nginx
   server {
       # tier-1 forwards raw TLS here WITH the PROXY header.
       listen 127.0.0.1:8500 ssl proxy_protocol;
       set_real_ip_from 127.0.0.1;
       real_ip_header   proxy_protocol;
       server_name push.brynify.me;

       ssl_certificate     /etc/letsencrypt/live/push.brynify.me/fullchain.pem;
       ssl_certificate_key /etc/letsencrypt/live/push.brynify.me/privkey.pem;

       location / {
           proxy_pass http://127.0.0.1:8787;
           proxy_set_header Host              $host;
           proxy_set_header X-Forwarded-For   $proxy_protocol_addr;
           proxy_set_header X-Forwarded-Proto https;
       }
   }
   ```

5. `sudo nginx -t && sudo systemctl reload nginx`

Then `RELAY_PUBLIC_BASE=https://push.brynify.me` and endpoints become
`https://push.brynify.me/push/<id>`.

Verify end to end:
```sh
curl -s https://push.brynify.me/healthz         # -> {"status":"ok"}
```

## Security notes

- The `.p8` is a secret: mode 600, only in `/etc/fastsm-push-relay/`, never in
  git (see `.gitignore`).
- Endpoint ids are 192-bit random and unguessable (the Web Push secrecy model).
- Set `RELAY_REGISTER_TOKEN` to a long random string if you want to gate
  `/register`; the app's `push_subscribe` sends the same value.
- Never place the user's account email in a subscription or relay request.
