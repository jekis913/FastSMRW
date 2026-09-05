// FastSMRW push relay.
//
// Bridges Mastodon Web Push to Apple Push Notification service (APNs). iOS can
// only be woken by APNs; Mastodon can only deliver Web Push. This tiny service
// sits between them:
//
//   1. The app registers its APNs device token (POST /register) and gets back a
//      unique, unguessable endpoint URL (/push/<id>).
//   2. The app subscribes that URL with its Mastodon server.
//   3. Mastodon POSTs an ENCRYPTED Web Push body to /push/<id> on each event.
//   4. We forward the (still-encrypted) body to APNs for the device's token, as
//      a mutable-content alert. The app's Notification Service Extension
//      decrypts it on-device.
//
// The relay never holds the Web Push decryption keys and never sees notification
// contents — it is a blind forwarder. Standard library only (ES256 JWT via
// crypto/ecdsa; HTTP/2 to APNs via net/http).
package main

import (
	"bytes"
	"crypto/ecdsa"
	"crypto/rand"
	"crypto/sha256"
	"crypto/x509"
	"encoding/base64"
	"encoding/json"
	"encoding/pem"
	"errors"
	"io"
	"log"
	"net/http"
	"os"
	"path/filepath"
	"strconv"
	"strings"
	"sync"
	"time"
)

// ---- configuration (from environment; see fastsm-push-relay.service) --------

type config struct {
	listen        string // address to listen on, e.g. 127.0.0.1:8787 (behind a TLS reverse proxy)
	publicBase    string // public URL this service is reachable at, e.g. https://brynify.me/fastsm-push
	apnsKeyPath   string // path to the APNs .p8 auth key
	apnsKeyID     string // the APNs key id (10 chars)
	apnsTeamID    string // Apple developer Team id
	apnsBundleID  string // the app's bundle id -> apns-topic
	storePath     string // JSON file mapping endpoint id -> device
	registerToken string // optional shared secret required on /register (Bearer)
}

func loadConfig() config {
	env := func(k, def string) string {
		if v := os.Getenv(k); v != "" {
			return v
		}
		return def
	}
	return config{
		listen:        env("RELAY_LISTEN", "127.0.0.1:8787"),
		publicBase:    strings.TrimRight(env("RELAY_PUBLIC_BASE", ""), "/"),
		apnsKeyPath:   env("APNS_KEY_PATH", ""),
		apnsKeyID:     env("APNS_KEY_ID", ""),
		apnsTeamID:    env("APNS_TEAM_ID", ""),
		apnsBundleID:  env("APNS_BUNDLE_ID", "me.masonasons.FastSMRW"),
		storePath:     env("RELAY_STORE", "/var/lib/fastsm-push-relay/devices.json"),
		registerToken: env("RELAY_REGISTER_TOKEN", ""),
	}
}

// ---- device store (endpoint id -> APNs device) ------------------------------

type device struct {
	Token   string `json:"token"`       // APNs device token (hex)
	Env     string `json:"env"`         // "sandbox" or "production"
	Updated int64  `json:"updated"`     // unix seconds
}

type store struct {
	mu   sync.Mutex
	path string
	m    map[string]device
}

func newStore(path string) (*store, error) {
	s := &store{path: path, m: map[string]device{}}
	if err := os.MkdirAll(filepath.Dir(path), 0o750); err != nil {
		return nil, err
	}
	if b, err := os.ReadFile(path); err == nil {
		_ = json.Unmarshal(b, &s.m)
	} else if !errors.Is(err, os.ErrNotExist) {
		return nil, err
	}
	return s, nil
}

func (s *store) put(id string, d device) error {
	s.mu.Lock()
	defer s.mu.Unlock()
	s.m[id] = d
	return s.flushLocked()
}

func (s *store) get(id string) (device, bool) {
	s.mu.Lock()
	defer s.mu.Unlock()
	d, ok := s.m[id]
	return d, ok
}

func (s *store) delete(id string) error {
	s.mu.Lock()
	defer s.mu.Unlock()
	delete(s.m, id)
	return s.flushLocked()
}

func (s *store) flushLocked() error {
	b, err := json.MarshalIndent(s.m, "", "  ")
	if err != nil {
		return err
	}
	tmp := s.path + ".tmp"
	if err := os.WriteFile(tmp, b, 0o640); err != nil {
		return err
	}
	return os.Rename(tmp, s.path)
}

// ---- APNs client (ES256 JWT auth over HTTP/2) -------------------------------

type apns struct {
	cfg    config
	key    *ecdsa.PrivateKey
	client *http.Client

	mu       sync.Mutex
	jwt      string
	jwtIssAt time.Time
}

func newAPNS(cfg config) (*apns, error) {
	pemBytes, err := os.ReadFile(cfg.apnsKeyPath)
	if err != nil {
		return nil, err
	}
	block, _ := pem.Decode(pemBytes)
	if block == nil {
		return nil, errors.New("apns key: not PEM")
	}
	parsed, err := x509.ParsePKCS8PrivateKey(block.Bytes)
	if err != nil {
		return nil, err
	}
	key, ok := parsed.(*ecdsa.PrivateKey)
	if !ok {
		return nil, errors.New("apns key: not an ECDSA P-256 key")
	}
	return &apns{
		cfg:    cfg,
		key:    key,
		client: &http.Client{Timeout: 15 * time.Second},
	}, nil
}

func b64url(b []byte) string { return base64.RawURLEncoding.EncodeToString(b) }

// token returns a cached bearer JWT, regenerating it if older than ~40 minutes
// (APNs accepts a token for up to an hour).
func (a *apns) token() (string, error) {
	a.mu.Lock()
	defer a.mu.Unlock()
	if a.jwt != "" && time.Since(a.jwtIssAt) < 40*time.Minute {
		return a.jwt, nil
	}
	now := time.Now()
	header := b64url([]byte(`{"alg":"ES256","kid":"` + a.cfg.apnsKeyID + `"}`))
	claims := b64url([]byte(`{"iss":"` + a.cfg.apnsTeamID + `","iat":` + strconv.FormatInt(now.Unix(), 10) + `}`))
	signingInput := header + "." + claims
	digest := sha256.Sum256([]byte(signingInput))
	r, s, err := ecdsa.Sign(rand.Reader, a.key, digest[:])
	if err != nil {
		return "", err
	}
	// JWS ES256 signature is the raw r||s, each left-padded to 32 bytes.
	sig := make([]byte, 64)
	r.FillBytes(sig[0:32])
	s.FillBytes(sig[32:64])
	a.jwt = signingInput + "." + b64url(sig)
	a.jwtIssAt = now
	return a.jwt, nil
}

func (a *apns) host(env string) string {
	if env == "production" {
		return "https://api.push.apple.com"
	}
	return "https://api.sandbox.push.apple.com" // default: development/dev-signed tokens
}

// send delivers one APNs push. Returns the APNs status and (on failure) the
// "reason" APNs gives, so the caller can drop dead device tokens.
func (a *apns) send(d device, payload []byte) (int, string, error) {
	jwt, err := a.token()
	if err != nil {
		return 0, "", err
	}
	url := a.host(d.Env) + "/3/device/" + d.Token
	req, err := http.NewRequest(http.MethodPost, url, bytes.NewReader(payload))
	if err != nil {
		return 0, "", err
	}
	req.Header.Set("authorization", "bearer "+jwt)
	req.Header.Set("apns-topic", a.cfg.apnsBundleID)
	req.Header.Set("apns-push-type", "alert")
	req.Header.Set("apns-priority", "10")
	resp, err := a.client.Do(req)
	if err != nil {
		return 0, "", err
	}
	defer resp.Body.Close()
	body, _ := io.ReadAll(resp.Body)
	reason := ""
	if resp.StatusCode >= 300 {
		var r struct {
			Reason string `json:"reason"`
		}
		_ = json.Unmarshal(body, &r)
		reason = r.Reason
	}
	return resp.StatusCode, reason, nil
}

// ---- HTTP handlers ----------------------------------------------------------

type server struct {
	cfg   config
	store *store
	apns  *apns
}

func randomID() string {
	b := make([]byte, 24)
	_, _ = rand.Read(b)
	return base64.RawURLEncoding.EncodeToString(b)
}

// POST /register {device_token, environment, endpoint_id?} -> {endpoint}
func (s *server) handleRegister(w http.ResponseWriter, r *http.Request) {
	if r.Method != http.MethodPost {
		http.Error(w, "method not allowed", http.StatusMethodNotAllowed)
		return
	}
	if s.cfg.registerToken != "" {
		if r.Header.Get("Authorization") != "Bearer "+s.cfg.registerToken {
			http.Error(w, "unauthorized", http.StatusUnauthorized)
			return
		}
	}
	var body struct {
		DeviceToken string `json:"device_token"`
		Environment string `json:"environment"`
		EndpointID  string `json:"endpoint_id"`
	}
	if err := json.NewDecoder(io.LimitReader(r.Body, 8192)).Decode(&body); err != nil {
		http.Error(w, "bad json", http.StatusBadRequest)
		return
	}
	if body.DeviceToken == "" {
		http.Error(w, "device_token required", http.StatusBadRequest)
		return
	}
	env := body.Environment
	if env != "production" {
		env = "sandbox"
	}
	id := body.EndpointID
	if id == "" {
		id = randomID()
	}
	if err := s.store.put(id, device{Token: body.DeviceToken, Env: env, Updated: time.Now().Unix()}); err != nil {
		http.Error(w, "store error", http.StatusInternalServerError)
		return
	}
	writeJSON(w, http.StatusOK, map[string]string{"endpoint": s.cfg.publicBase + "/push/" + id})
}

// POST /push/<id> : the encrypted Web Push body from Mastodon.
func (s *server) handlePush(w http.ResponseWriter, r *http.Request) {
	if r.Method != http.MethodPost {
		http.Error(w, "method not allowed", http.StatusMethodNotAllowed)
		return
	}
	id := strings.TrimPrefix(r.URL.Path, "/push/")
	if id == "" || strings.Contains(id, "/") {
		http.NotFound(w, r)
		return
	}
	d, ok := s.store.get(id)
	if !ok {
		http.NotFound(w, r) // unknown endpoint -> Mastodon eventually drops the subscription
		return
	}
	encrypted, err := io.ReadAll(io.LimitReader(r.Body, 6144)) // APNs payload cap is ~4KB
	if err != nil {
		http.Error(w, "read error", http.StatusBadRequest)
		return
	}

	// The full payload carries the encrypted blob so the on-device Notification
	// Service Extension can decrypt it. "aps.alert" is a fallback shown if the
	// extension can't run or times out. "ce" is the content-encoding; for the
	// older "aesgcm" scheme the salt and server key travel in the Encryption /
	// Crypto-Key headers, so forward those too ("enc" / "ck").
	contentEncoding := r.Header.Get("Content-Encoding")
	enc := r.Header.Get("Encryption")
	ck := r.Header.Get("Crypto-Key")
	payload := map[string]any{
		"aps": map[string]any{
			"alert":           map[string]string{"title": "FastSM", "body": "New notification"},
			"mutable-content": 1,
			"sound":           "default",
		},
		"m":  base64.RawURLEncoding.EncodeToString(encrypted),
		"ce": contentEncoding,
	}
	if enc != "" {
		payload["enc"] = enc
	}
	if ck != "" {
		payload["ck"] = ck
	}
	body, _ := json.Marshal(payload)

	status, reason, err := s.apns.send(d, body)
	if err != nil {
		log.Printf("push %s: apns error: %v", id, err)
		http.Error(w, "upstream error", http.StatusBadGateway)
		return
	}
	if status == http.StatusGone || reason == "BadDeviceToken" || reason == "Unregistered" {
		// The device token is dead — forget it so we stop trying.
		_ = s.store.delete(id)
		log.Printf("push %s: device gone (%s), removed", id, reason)
		http.Error(w, "gone", http.StatusGone)
		return
	}
	if status >= 300 {
		log.Printf("push %s: apns rejected: %d %s", id, status, reason)
		http.Error(w, reason, http.StatusBadGateway)
		return
	}
	w.WriteHeader(http.StatusCreated)
}

func (s *server) handleHealth(w http.ResponseWriter, r *http.Request) {
	writeJSON(w, http.StatusOK, map[string]string{"status": "ok"})
}

// ---- small helpers ----------------------------------------------------------

func writeJSON(w http.ResponseWriter, status int, v any) {
	w.Header().Set("Content-Type", "application/json")
	w.WriteHeader(status)
	_ = json.NewEncoder(w).Encode(v)
}

func main() {
	cfg := loadConfig()
	if cfg.publicBase == "" || cfg.apnsKeyPath == "" || cfg.apnsKeyID == "" || cfg.apnsTeamID == "" {
		log.Fatal("missing required config: RELAY_PUBLIC_BASE, APNS_KEY_PATH, APNS_KEY_ID, APNS_TEAM_ID")
	}
	st, err := newStore(cfg.storePath)
	if err != nil {
		log.Fatalf("store: %v", err)
	}
	ap, err := newAPNS(cfg)
	if err != nil {
		log.Fatalf("apns: %v", err)
	}
	s := &server{cfg: cfg, store: st, apns: ap}

	mux := http.NewServeMux()
	mux.HandleFunc("/register", s.handleRegister)
	mux.HandleFunc("/push/", s.handlePush)
	mux.HandleFunc("/healthz", s.handleHealth)

	srv := &http.Server{
		Addr:              cfg.listen,
		Handler:           mux,
		ReadHeaderTimeout: 10 * time.Second,
	}
	log.Printf("fastsm-push-relay listening on %s (public %s)", cfg.listen, cfg.publicBase)
	log.Fatal(srv.ListenAndServe())
}
