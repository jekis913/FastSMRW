//
//  WebPushDecrypt.swift
//
//  RFC 8291 (Web Push message encryption) over RFC 8188 (aes128gcm content
//  coding), implemented with CryptoKit. The Notification Service Extension uses
//  this to decrypt an incoming push on-device with the keypair in WebPushKeys.
//
//  Body layout (RFC 8188 §2.1):
//      salt(16) | rs(4, uint32) | idlen(1) | keyid(idlen) | ciphertext+tag
//  For Web Push the keyid is the sender's ephemeral P-256 public key (65 bytes).
//

import CryptoKit
import Foundation

enum WebPush {
    /// Decrypt an aes128gcm Web Push body. Returns the plaintext (the Mastodon
    /// push JSON), or nil if the body is malformed or authentication fails.
    static func decrypt(body: Data, privateKey: P256.KeyAgreement.PrivateKey,
                        authSecret: Data) -> Data? {
        guard body.count > 21 else { return nil }
        let salt = body.subdata(in: 0..<16)
        let idlen = Int(body[20])
        guard idlen == 65 else { return nil } // a P-256 uncompressed point
        let headerEnd = 21 + idlen
        guard body.count > headerEnd + 16 else { return nil }
        let serverPubData = body.subdata(in: 21..<headerEnd)
        let ciphertextAndTag = body.subdata(in: headerEnd..<body.count)

        guard let serverPub = try? P256.KeyAgreement.PublicKey(x963Representation: serverPubData),
              let shared = try? privateKey.sharedSecretFromKeyAgreement(with: serverPub)
        else { return nil }

        // RFC 8291 §3.3 — IKM = HKDF(salt: auth, ikm: ecdh,
        //   info: "WebPush: info"\0 || ua_public || as_public), 32 bytes.
        let uaPublic = privateKey.publicKey.x963Representation // 65 bytes
        var keyInfo = Data("WebPush: info".utf8)
        keyInfo.append(0x00)
        keyInfo.append(uaPublic)
        keyInfo.append(serverPubData)
        let ikm = shared.hkdfDerivedSymmetricKey(using: SHA256.self, salt: authSecret,
                                                 sharedInfo: keyInfo, outputByteCount: 32)

        // RFC 8188 §2.2 — CEK and nonce from IKM, salted with the header salt.
        let cekInfo = Data("Content-Encoding: aes128gcm".utf8) + Data([0x00])
        let nonceInfo = Data("Content-Encoding: nonce".utf8) + Data([0x00])
        let cek = HKDF<SHA256>.deriveKey(inputKeyMaterial: ikm, salt: salt, info: cekInfo,
                                         outputByteCount: 16)
        let nonceKey = HKDF<SHA256>.deriveKey(inputKeyMaterial: ikm, salt: salt, info: nonceInfo,
                                              outputByteCount: 12)
        let nonceData = nonceKey.withUnsafeBytes { Data($0) }

        let tag = ciphertextAndTag.suffix(16)
        let ct = ciphertextAndTag.prefix(ciphertextAndTag.count - 16)
        guard let nonce = try? AES.GCM.Nonce(data: nonceData),
              let sealed = try? AES.GCM.SealedBox(nonce: nonce, ciphertext: ct, tag: tag),
              let plain = try? AES.GCM.open(sealed, using: cek)
        else { return nil }
        return stripPadding(plain)
    }

    /// RFC 8188 padding: a single record's plaintext is `data || 0x02 || 0x00*`
    /// (0x01 for a non-final record). Trim the zero padding and the delimiter.
    private static func stripPadding(_ data: Data) -> Data {
        var d = data
        while d.last == 0x00 { d.removeLast() }
        if d.last == 0x02 || d.last == 0x01 { d.removeLast() }
        return d
    }

    /// Decrypt the older "aesgcm" content-encoding (draft-ietf-webpush-encryption-04),
    /// which Mastodon's web-push library uses. The salt and the server's public
    /// key arrive in the Encryption / Crypto-Key HTTP headers (forwarded by the
    /// relay), and the body is just the ciphertext (2-byte padding-length prefix
    /// after decryption). Returns the plaintext (the Mastodon push JSON).
    static func decryptAESGCM(ciphertext: Data, salt: Data, serverPublic: Data,
                              privateKey: P256.KeyAgreement.PrivateKey, authSecret: Data) -> Data? {
        guard ciphertext.count > 16,
              let serverPub = try? P256.KeyAgreement.PublicKey(x963Representation: serverPublic),
              let shared = try? privateKey.sharedSecretFromKeyAgreement(with: serverPub)
        else { return nil }
        let uaPublic = privateKey.publicKey.x963Representation // 65 bytes

        // IKM = HKDF(salt: auth, ikm: ecdh, info: "Content-Encoding: auth"\0), 32.
        let authInfo = Data("Content-Encoding: auth".utf8) + Data([0x00])
        let ikm = shared.hkdfDerivedSymmetricKey(using: SHA256.self, salt: authSecret,
                                                 sharedInfo: authInfo, outputByteCount: 32)
        // context = "P-256"\0 || len16(ua) || ua || len16(server) || server.
        var context = Data("P-256".utf8)
        context.append(0x00)
        context.append(UInt8(uaPublic.count >> 8)); context.append(UInt8(uaPublic.count & 0xff))
        context.append(uaPublic)
        context.append(UInt8(serverPublic.count >> 8)); context.append(UInt8(serverPublic.count & 0xff))
        context.append(serverPublic)

        let cekInfo = Data("Content-Encoding: aesgcm".utf8) + Data([0x00]) + context
        let nonceInfo = Data("Content-Encoding: nonce".utf8) + Data([0x00]) + context
        let cek = HKDF<SHA256>.deriveKey(inputKeyMaterial: ikm, salt: salt, info: cekInfo,
                                         outputByteCount: 16)
        let nonceKey = HKDF<SHA256>.deriveKey(inputKeyMaterial: ikm, salt: salt, info: nonceInfo,
                                              outputByteCount: 12)
        let nonceData = nonceKey.withUnsafeBytes { Data($0) }

        let tag = ciphertext.suffix(16)
        let ct = ciphertext.prefix(ciphertext.count - 16)
        guard let nonce = try? AES.GCM.Nonce(data: nonceData),
              let sealed = try? AES.GCM.SealedBox(nonce: nonce, ciphertext: ct, tag: tag),
              let plain = try? AES.GCM.open(sealed, using: cek)
        else { return nil }

        // aesgcm padding: a 2-octet big-endian pad length, that many zero octets,
        // then the content.
        guard plain.count >= 2 else { return nil }
        let padLen = Int(plain[plain.startIndex]) << 8 | Int(plain[plain.startIndex + 1])
        let start = 2 + padLen
        guard plain.count >= start else { return nil }
        return plain.subdata(in: (plain.startIndex + start)..<plain.endIndex)
    }
}
