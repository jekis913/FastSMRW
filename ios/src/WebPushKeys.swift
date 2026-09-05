//
//  WebPushKeys.swift
//
//  The device's Web Push keypair (RFC 8291): a stable P-256 key and a 16-byte
//  auth secret, kept in the Keychain so Mastodon keeps encrypting to the same
//  keys across launches. Crypto lives platform-side here because the C++ core
//  links no crypto library on any platform — the same way HTTP transport is a
//  per-platform shim. The core still composes the notification text.
//
//  The public halves (p256dh, auth) go to Mastodon via the core's push_subscribe
//  command; the private key + auth secret stay on-device for the Notification
//  Service Extension to decrypt incoming pushes (P4).
//

import CryptoKit
import Foundation
import Security

enum WebPushKeys {
    /// A Keychain access group could be added here later so the app extension
    /// shares these items; for now both live under the app's default group.
    private static let service = "me.masonasons.FastSMRW.webpush"
    private static let privKeyAccount = "p256-private"
    private static let authAccount = "auth-secret"

    /// The subscription's public values, base64url (unpadded).
    struct PublicKeys {
        let p256dh: String // uncompressed public point, 0x04 || X || Y (65 bytes)
        let auth: String   // the 16-byte auth secret
    }

    /// Load or create the keypair + auth secret; returns the public parts to
    /// hand to Mastodon.
    static func ensure() -> PublicKeys {
        let priv = loadOrCreatePrivateKey()
        let auth = loadOrCreateAuth()
        return PublicKeys(p256dh: base64url(priv.publicKey.x963Representation),
                          auth: base64url(auth))
    }

    /// The private key, for decrypting incoming pushes (used by the Notification
    /// Service Extension at P4). nil if not yet created.
    static func privateKey() -> P256.KeyAgreement.PrivateKey? {
        guard let raw = keychainRead(privKeyAccount) else { return nil }
        return try? P256.KeyAgreement.PrivateKey(rawRepresentation: raw)
    }

    static func authSecret() -> Data? { keychainRead(authAccount) }

    // MARK: - key material

    private static func loadOrCreatePrivateKey() -> P256.KeyAgreement.PrivateKey {
        if let raw = keychainRead(privKeyAccount),
           let key = try? P256.KeyAgreement.PrivateKey(rawRepresentation: raw) {
            return key
        }
        let key = P256.KeyAgreement.PrivateKey()
        keychainWrite(privKeyAccount, key.rawRepresentation)
        return key
    }

    private static func loadOrCreateAuth() -> Data {
        if let a = keychainRead(authAccount), a.count == 16 { return a }
        var bytes = [UInt8](repeating: 0, count: 16)
        _ = SecRandomCopyBytes(kSecRandomDefault, bytes.count, &bytes)
        let data = Data(bytes)
        keychainWrite(authAccount, data)
        return data
    }

    private static func base64url(_ d: Data) -> String {
        d.base64EncodedString()
            .replacingOccurrences(of: "+", with: "-")
            .replacingOccurrences(of: "/", with: "_")
            .replacingOccurrences(of: "=", with: "")
    }

    // MARK: - Keychain

    private static func baseQuery(_ account: String) -> [String: Any] {
        [kSecClass as String: kSecClassGenericPassword,
         kSecAttrService as String: service,
         kSecAttrAccount as String: account]
    }

    private static func keychainRead(_ account: String) -> Data? {
        var q = baseQuery(account)
        q[kSecReturnData as String] = true
        q[kSecMatchLimit as String] = kSecMatchLimitOne
        var out: AnyObject?
        guard SecItemCopyMatching(q as CFDictionary, &out) == errSecSuccess else { return nil }
        return out as? Data
    }

    private static func keychainWrite(_ account: String, _ data: Data) {
        SecItemDelete(baseQuery(account) as CFDictionary)
        var q = baseQuery(account)
        q[kSecValueData as String] = data
        // AfterFirstUnlock so the background Notification Service Extension can
        // read the key to decrypt a push.
        q[kSecAttrAccessible as String] = kSecAttrAccessibleAfterFirstUnlock
        SecItemAdd(q as CFDictionary, nil)
    }
}
