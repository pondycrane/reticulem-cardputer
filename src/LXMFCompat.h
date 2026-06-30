#ifndef RETICULEM_LXMFCOMPAT_H
#define RETICULEM_LXMFCOMPAT_H

#include <microReticulum.h>
#include <cstdint>
#include <string>

//
// LXMFCompat — Lightweight eXtensible Message Format compatibility layer
//
// Provides packing/unpacking of LXMF wire format messages and announce
// app_data encoding/decoding for Sideband and other LXMF clients.
//
// Wire format (from Python LXMessage.pack()):
//   packed = destination_hash(16 bytes)
//          + source_hash(16 bytes)
//          + Ed25519_signature(64 bytes)
//          + msgpack([timestamp(double), title(bin), content(bin), fields(map)])
//
// Total overhead: 16 + 16 + 64 = 96 bytes before payload.
//

namespace LXMFCompat {

// LXMF aspect constants
constexpr const char* APP_NAME         = "lxmf";
constexpr const char* DELIVERY_ASPECT  = "delivery";

// Lengths derived from Reticulum constants
// RNS::Type::Reticulum::TRUNCATED_HASHLENGTH/8  = 128/8 = 16
// RNS::Type::Identity::SIGLENGTH/8               = 512/8 = 64
constexpr size_t DESTINATION_LENGTH  = 16;
constexpr size_t SIGNATURE_LENGTH    = 64;

//
// Parsed fields of an incoming LXMF message
//
struct LXMessage {
    RNS::Bytes  destHash;     // 16-byte destination identity hash
    RNS::Bytes  srcHash;      // 16-byte source identity hash
    RNS::Bytes  signature;    // 64-byte Ed25519 signature
    double      timestamp;    // Seconds since epoch (or startup)
    std::string title;        // Message title (UTF-8)
    std::string content;      // Message body (UTF-8)
};

//
// Pack an outgoing message into LXMF wire format bytes.
//
// @param source      The local RNS::Identity (must hold a private key)
// @param destHash    Full 32-byte identity hash of the recipient
// @param title       Message title (optional, may be nullptr or empty)
// @param content     Message body (UTF-8)
// @return            Complete LXMF wire-format bytes suitable for packet payload
//
RNS::Bytes packMessage(
    const RNS::Identity& source,
    const RNS::Bytes& destHash,
    const char* title,
    const char* content
);

//
// Unpack an incoming byte buffer into an LXMessage struct.
//
// Detects whether the payload includes a destination hash (DIRECT delivery
// format) or omits it (OPPORTUNISTIC delivery format).  Returns true on
// successful parse; false if the data does not look like a valid LXMF message.
//
// @param data    Raw packet payload bytes (may or may not include dest hash)
// @param out     Filled with parsed fields on success
// @return        true if the message was successfully parsed
//
bool unpackMessage(const RNS::Bytes& data, LXMessage& out);

//
// Verify the Ed25519 signature on a parsed LXMessage.
//
// Reconstructs the signed blob (hashed_part + message_hash) and calls
// sourceIdentity.validate().
//
// NOTE: OPPORTUNISTIC messages (where destHash is empty) cannot be fully
// verified because the destination hash is needed to reconstruct the signed
// data. In those cases a zero-filled placeholder is used, but verification
// will fail. Callers should check msg.destHash.size() and treat an empty
// destHash as "not verifiable" rather than "invalid".
//
// @param msg                Parsed LXMessage; destHash may be empty for
//                           OPPORTUNISTIC delivery (non-verifiable case)
// @param sourceIdentity     RNS::Identity of the sender (recalled via RNS::Identity::recall)
// @return                   true if signature is valid
//
bool verifySignature(const LXMessage& msg, const RNS::Identity& sourceIdentity);

//
// Encode announce app_data in LXMF v0.5.0+ format:
//   MsgPack([displayName, null, []])
//
// Sideband detects this as a valid LXMF peer and shows the display name.
//
// @param displayName   UTF-8 display name (e.g. "Cardputer")
// @return              MsgPack-encoded app_data bytes
//
RNS::Bytes encodeAnnounceData(const char* displayName);

//
// Decode announce app_data to extract the display name.
//
// Handles both LXMF v0.5.0+ MsgPack format and legacy plain-UTF-8 format
// (from older ReticuleM instances).
//
// @param appData       Raw app_data bytes from the announce
// @param nameOut       Output buffer for the display name
// @param nameOutSize   Size of nameOut buffer
// @return              true if a display name was successfully extracted
//
bool decodeAnnounceData(const RNS::Bytes& appData, char* nameOut, size_t nameOutSize);

} // namespace LXMFCompat

#endif // RETICULEM_LXMFCOMPAT_H
