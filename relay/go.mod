module github.com/masonasons/fastsm-push-relay

// Standard library only — ES256 JWT signing (crypto/ecdsa + crypto/x509 to
// parse the APNs .p8) and HTTP/2 to APNs (net/http speaks h2 over TLS by
// default) need no third-party packages.
go 1.22
