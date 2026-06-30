#include "LXMFCompat.h"

#include <cstring>
#include <cmath>
#include <algorithm>

namespace LXMFCompat {

// ------------------------------------------------------------------
// MsgPack low-level helpers
//
// We handle only the subset of MsgPack needed for LXMF:
//   - fixarray  (0x90-0x9f)  → 4-element array
//   - fixmap    (0x80-0x8f)  → empty or small map
//   - float64   (0xcb)       → 8-byte IEEE 754 double
//   - nil       (0xc0)
//   - bin8      (0xc4)       → 1-byte length + data
//   - bin16     (0xc5)       → 2-byte length + data (big-endian)
//
// Using these primitives avoids the template-heavy MsgPack::Unpacker
// on the MCU and gives us exact control over memory layout.
// ------------------------------------------------------------------

static inline bool isFixArray(uint8_t b) { return (b & 0xF0) == 0x90; }
static inline bool isFixMap(uint8_t b)   { return (b & 0xF0) == 0x80; }
static inline uint8_t fixLen(uint8_t b)   { return b & 0x0F; }

// Write a fixarray header (0x90 + count) to a buffer
static inline void writeFixArray(uint8_t* buf, uint8_t count) {
    buf[0] = 0x90 | (count & 0x0F);
}

// Write a fixmap header (0x80 + count)
static inline void writeFixMap(uint8_t* buf, uint8_t count) {
    buf[0] = 0x80 | (count & 0x0F);
}

// Write nil (0xc0)
static inline void writeNil(uint8_t* buf) {
    buf[0] = 0xC0;
}

// Write float64 header + 8-byte big-endian IEEE 754 double
static void writeFloat64(uint8_t* buf, double value) {
    buf[0] = 0xCB;
    // Interpret the double as a uint64_t for portable byte extraction
    union { double d; uint64_t u; } u;
    u.d = value;
    for (int i = 7; i >= 0; --i) {
        buf[1 + (7 - i)] = (uint8_t)((u.u >> (8 * i)) & 0xFF);
    }
}

// Read a float64 from bytes at cursor, advance past it
static double readFloat64(const uint8_t* data, size_t& pos, size_t maxLen) {
    if (pos + 9 > maxLen) return 0.0;
    ++pos; // skip 0xCB
    union { uint64_t u; double d; } u;
    u.u = 0;
    for (int i = 0; i < 8; ++i) {
        u.u = (u.u << 8) | data[pos++];
    }
    return u.d;
}

// Write bin8 header (0xc4) + length + data
static void writeBin8(uint8_t* out, const uint8_t* data, uint8_t len) {
    out[0] = 0xC4;
    out[1] = len;
    if (len > 0) memcpy(out + 2, data, len);
}

// Write bin16 header (0xc5) + 2-byte big-endian length + data
static void writeBin16(uint8_t* out, const uint8_t* data, uint16_t len) {
    out[0] = 0xC5;
    out[1] = (uint8_t)((len >> 8) & 0xFF);
    out[2] = (uint8_t)(len & 0xFF);
    if (len > 0) memcpy(out + 3, data, len);
}

// Determine bin header size for a given data length
static inline size_t binHeaderSize(uint16_t len) {
    return (len <= 255) ? 2 : 3;  // bin8 = 2, bin16 = 3
}

//
// Compute total MsgPack payload size for packMessage()
//   = 1 (fixarray 4)
//   + 9 (float64)
//   + binHeaderSize(strlen(title)) + strlen(title)
//   + binHeaderSize(strlen(content)) + strlen(content)
//   + 1 (fixmap 0)
//
static size_t msgPackPayloadSize(const char* title, const char* content) {
    size_t tlen = (title && title[0]) ? strlen(title) : 0;
    size_t clen = (content && content[0]) ? strlen(content) : 0;
    size_t sz = 1;                          // fixarray(4)
    sz += 9;                                // float64(timestamp)
    sz += binHeaderSize((uint16_t)tlen) + tlen;   // bin(title)
    sz += binHeaderSize((uint16_t)clen) + clen;   // bin(content)
    sz += 1;                                // fixmap(0)
    return sz;
}

// ------------------------------------------------------------------
// packMessage — build LXMF wire-format bytes
// ------------------------------------------------------------------
RNS::Bytes packMessage(
    const RNS::Identity& source,
    const RNS::Bytes& destHash,
    const char* title,
    const char* content)
{
    // Normalise inputs
    if (!title) title = "";
    if (!content) content = "";
    size_t tlen = strlen(title);
    size_t clen = strlen(content);

    // Truncated dest/source hashes (16 bytes each)
    RNS::Bytes truncatedDest = RNS::Identity::truncated_hash(destHash);
    RNS::Bytes truncatedSrc  = source.hash();  // Already 16 bytes

    // ---- Build MsgPack payload ----
    size_t payloadLen = msgPackPayloadSize(title, content);
    std::vector<uint8_t> payload(payloadLen);
    size_t pos = 0;

    // fixarray(4)
    payload[pos++] = 0x94;

    // float64(timestamp) — using OS uptime seconds (consistent with Python style)
    double ts = RNS::Utilities::OS::time();
    writeFloat64(&payload[pos], ts);
    pos += 9;

    // bin(title)
    if (tlen <= 255) {
        writeBin8(&payload[pos], (const uint8_t*)title, (uint8_t)tlen);
        pos += 2 + tlen;
    } else {
        uint16_t len16 = (uint16_t)(tlen > 65535 ? 65535 : tlen);
        writeBin16(&payload[pos], (const uint8_t*)title, len16);
        pos += 3 + len16;
    }

    // bin(content)
    if (clen <= 255) {
        writeBin8(&payload[pos], (const uint8_t*)content, (uint8_t)clen);
        pos += 2 + clen;
    } else {
        uint16_t len16 = (uint16_t)(clen > 65535 ? 65535 : clen);
        writeBin16(&payload[pos], (const uint8_t*)content, len16);
        pos += 3 + len16;
    }

    // fixmap(0) — empty fields
    payload[pos++] = 0x80;

    // ---- Compute signature ----
    // hashed_part = truncatedDest + truncatedSrc + msgpack_payload
    // message_hash = SHA-256(hashed_part)
    // signed_part = hashed_part + message_hash
    // signature = source.sign(signed_part)

    RNS::Bytes hashedPart;
    hashedPart.append(truncatedDest);
    hashedPart.append(truncatedSrc);
    hashedPart.append(payload.data(), payload.size());

    RNS::Bytes messageHash = RNS::Identity::full_hash(hashedPart);

    RNS::Bytes signedPart;
    signedPart.append(hashedPart);
    signedPart.append(messageHash);

    RNS::Bytes signature = source.sign(signedPart);

    // ---- Assemble packed message ----
    // packed = truncatedDest + truncatedSrc + signature + msgpack_payload
    RNS::Bytes packed;
    packed.append(truncatedDest);
    packed.append(truncatedSrc);
    packed.append(signature);
    packed.append(payload.data(), payload.size());

    return packed;
}

// ------------------------------------------------------------------
// MsgPack skip helper — advance past one MsgPack element
// Returns true on success; pos is advanced regardless on success.
// ------------------------------------------------------------------
static bool skipMsgPackElement(const uint8_t* data, size_t& pos, size_t maxLen) {
    if (pos >= maxLen) return false;
    uint8_t b = data[pos++];

    // Positive fixint 0x00-0x7f
    if (b <= 0x7F) return true;
    // Negative fixint 0xe0-0xff
    if (b >= 0xE0) return true;
    // fixstr 0xa0-0xbf
    if (b >= 0xA0 && b <= 0xBF) {
        uint8_t len = b & 0x1F;
        if (pos + len > maxLen) return false;
        pos += len;
        return true;
    }
    // fixarray 0x90-0x9f
    if (b >= 0x90 && b <= 0x9F) {
        uint8_t count = b & 0x0F;
        for (uint8_t i = 0; i < count; ++i) {
            if (!skipMsgPackElement(data, pos, maxLen)) return false;
        }
        return true;
    }
    // fixmap 0x80-0x8f
    if (b >= 0x80 && b <= 0x8F) {
        uint8_t count = b & 0x0F;
        for (uint8_t i = 0; i < count * 2; ++i) { // key + value pairs
            if (!skipMsgPackElement(data, pos, maxLen)) return false;
        }
        return true;
    }
    // nil 0xc0
    if (b == 0xC0) return true;
    // false 0xc2, true 0xc3
    if (b == 0xC2 || b == 0xC3) return true;
    // bin8 0xc4
    if (b == 0xC4) {
        if (pos >= maxLen) return false;
        uint8_t len = data[pos++];
        if (pos + len > maxLen) return false;
        pos += len;
        return true;
    }
    // bin16 0xc5
    if (b == 0xC5) {
        if (pos + 2 > maxLen) return false;
        uint16_t len = ((uint16_t)data[pos] << 8) | data[pos + 1];
        pos += 2;
        if (pos + len > maxLen) return false;
        pos += len;
        return true;
    }
    // bin32 0xc6
    if (b == 0xC6) {
        if (pos + 4 > maxLen) return false;
        uint32_t len = ((uint32_t)data[pos] << 24) | ((uint32_t)data[pos+1] << 16)
                     | ((uint32_t)data[pos+2] << 8) | data[pos+3];
        pos += 4;
        if (pos + len > maxLen) return false;
        pos += len;
        return true;
    }
    // float32 0xca
    if (b == 0xCA) { pos += 4; return (pos <= maxLen); }
    // float64 0xcb
    if (b == 0xCB) { pos += 8; return (pos <= maxLen); }
    // uint8 0xcc, uint16 0xcd, uint32 0xce, uint64 0xcf
    if (b == 0xCC) { pos += 1; return (pos <= maxLen); }
    if (b == 0xCD) { pos += 2; return (pos <= maxLen); }
    if (b == 0xCE) { pos += 4; return (pos <= maxLen); }
    if (b == 0xCF) { pos += 8; return (pos <= maxLen); }
    // int8 0xd0, int16 0xd1, int32 0xd2, int64 0xd3
    if (b == 0xD0) { pos += 1; return (pos <= maxLen); }
    if (b == 0xD1) { pos += 2; return (pos <= maxLen); }
    if (b == 0xD2) { pos += 4; return (pos <= maxLen); }
    if (b == 0xD3) { pos += 8; return (pos <= maxLen); }
    // str8 0xd9, str16 0xda, str32 0xdb
    if (b == 0xD9) {
        if (pos >= maxLen) return false;
        uint8_t len = data[pos++];
        if (pos + len > maxLen) return false;
        pos += len;
        return true;
    }
    if (b == 0xDA) {
        if (pos + 2 > maxLen) return false;
        uint16_t len = ((uint16_t)data[pos] << 8) | data[pos+1];
        pos += 2;
        if (pos + len > maxLen) return false;
        pos += len;
        return true;
    }
    if (b == 0xDB) {
        if (pos + 4 > maxLen) return false;
        uint32_t len = ((uint32_t)data[pos] << 24) | ((uint32_t)data[pos+1] << 16)
                     | ((uint32_t)data[pos+2] << 8) | data[pos+3];
        pos += 4;
        if (pos + len > maxLen) return false;
        pos += len;
        return true;
    }
    // array16 0xdc, array32 0xdd
    if (b == 0xDC) {
        if (pos + 2 > maxLen) return false;
        uint16_t count = ((uint16_t)data[pos] << 8) | data[pos+1];
        pos += 2;
        for (uint16_t i = 0; i < count; ++i) {
            if (!skipMsgPackElement(data, pos, maxLen)) return false;
        }
        return true;
    }
    if (b == 0xDD) {
        if (pos + 4 > maxLen) return false;
        uint32_t count = ((uint32_t)data[pos] << 24) | ((uint32_t)data[pos+1] << 16)
                       | ((uint32_t)data[pos+2] << 8) | data[pos+3];
        pos += 4;
        for (uint32_t i = 0; i < count; ++i) {
            if (!skipMsgPackElement(data, pos, maxLen)) return false;
        }
        return true;
    }
    // map16 0xde, map32 0xdf
    if (b == 0xDE) {
        if (pos + 2 > maxLen) return false;
        uint16_t count = ((uint16_t)data[pos] << 8) | data[pos+1];
        pos += 2;
        for (uint16_t i = 0; i < count * 2; ++i) {
            if (!skipMsgPackElement(data, pos, maxLen)) return false;
        }
        return true;
    }
    if (b == 0xDF) {
        if (pos + 4 > maxLen) return false;
        uint32_t count = ((uint32_t)data[pos] << 24) | ((uint32_t)data[pos+1] << 16)
                       | ((uint32_t)data[pos+2] << 8) | data[pos+3];
        pos += 4;
        for (uint32_t i = 0; i < count * 2; ++i) {
            if (!skipMsgPackElement(data, pos, maxLen)) return false;
        }
        return true;
    }

    // Unknown type byte — cannot skip
    return false;
}

// Read a bin element: expects 0xc4 (bin8) or 0xc5 (bin16) at pos
// On success, extracts data into out string and advances pos.
static bool readBin(const uint8_t* data, size_t& pos, size_t maxLen, std::string& out) {
    if (pos >= maxLen) return false;
    uint8_t b = data[pos++];

    if (b == 0xC4) { // bin8
        if (pos >= maxLen) return false;
        uint8_t len = data[pos++];
        if (pos + len > maxLen) return false;
        out.assign((const char*)data + pos, len);
        pos += len;
        return true;
    }
    if (b == 0xC5) { // bin16
        if (pos + 2 > maxLen) return false;
        uint16_t len = ((uint16_t)data[pos] << 8) | data[pos+1];
        pos += 2;
        if (pos + len > maxLen) return false;
        out.assign((const char*)data + pos, len);
        pos += len;
        return true;
    }
    return false;
}

// ------------------------------------------------------------------
// unpackMessage — parse LXMF wire format into LXMessage struct
// ------------------------------------------------------------------
bool unpackMessage(const RNS::Bytes& data, LXMessage& out) {
    size_t len = data.size();
    if (len < DESTINATION_LENGTH + SIGNATURE_LENGTH + 4) {
        return false; // Too short for even minimal LXMF
    }

    const uint8_t* raw = data.data();

    // Determine whether this includes a destination hash at the front.
    // In OPPORTUNISTIC delivery, the destination hash is STRIPPED because
    // RNS already routes by it. The payload begins with src_hash.
    // In DIRECT delivery (over a Link), the full packed data is sent,
    // beginning with dest_hash.
    //
    // Heuristic: look for a fixarray(4) marker (0x94) at the expected
    // MsgPack payload offset. If found at offset 96 (DIRECT), use DIRECT.
    // If found at offset 80 (OPPORTUNISTIC), use OPPORTUNISTIC.
    // Fall back to trying both.

    size_t payloadOffset = 0;
    bool hasDestHash = false;

    // Check DIRECT format first (most common from Sideband)
    if (len >= DESTINATION_LENGTH * 2 + SIGNATURE_LENGTH + 1) {
        size_t directPayloadOff = DESTINATION_LENGTH * 2 + SIGNATURE_LENGTH; // 96
        if (directPayloadOff < len && (raw[directPayloadOff] == 0x94 || isFixArray(raw[directPayloadOff]))) {
            payloadOffset = directPayloadOff;
            hasDestHash = true;
        }
    }

    // If DIRECT heuristic fails, check OPPORTUNISTIC
    if (payloadOffset == 0 && len >= DESTINATION_LENGTH + SIGNATURE_LENGTH + 1) {
        size_t oppPayloadOff = DESTINATION_LENGTH + SIGNATURE_LENGTH; // 80
        if (oppPayloadOff < len && (raw[oppPayloadOff] == 0x94 || isFixArray(raw[oppPayloadOff]))) {
            payloadOffset = oppPayloadOff;
            hasDestHash = false;
        }
    }

    // If neither offset matched a fixarray marker, try DIRECT anyway
    // (might use array16/32)
    if (payloadOffset == 0) {
        payloadOffset = DESTINATION_LENGTH * 2 + SIGNATURE_LENGTH;
        if (payloadOffset < len) {
            hasDestHash = true;
        } else {
            return false;
        }
    }

    // Extract destination hash (if present) and source hash
    size_t offset = 0;
    if (hasDestHash) {
        out.destHash = data.mid(offset, DESTINATION_LENGTH);
        offset += DESTINATION_LENGTH;
    } else {
        out.destHash = RNS::Bytes(); // empty
    }
    out.srcHash   = data.mid(offset, DESTINATION_LENGTH);
    offset += DESTINATION_LENGTH;

    // Extract signature
    out.signature = data.mid(offset, SIGNATURE_LENGTH);
    offset += SIGNATURE_LENGTH;

    // Now parse MsgPack payload at payloadOffset
    size_t mpPos = payloadOffset;

    // Expect fixarray(4) or array16/array32
    if (mpPos >= len) return false;
    uint8_t arrayTag = raw[mpPos++];
    size_t arrayCount = 0;

    if (isFixArray(arrayTag)) {
        arrayCount = fixLen(arrayTag);
    } else if (arrayTag == 0xDC) { // array16
        if (mpPos + 2 > len) return false;
        arrayCount = ((uint16_t)raw[mpPos] << 8) | raw[mpPos+1];
        mpPos += 2;
    } else if (arrayTag == 0xDD) { // array32
        if (mpPos + 4 > len) return false;
        arrayCount = ((uint32_t)raw[mpPos] << 24) | ((uint32_t)raw[mpPos+1] << 16)
                   | ((uint32_t)raw[mpPos+2] << 8) | raw[mpPos+3];
        mpPos += 4;
    } else {
        return false; // Not an array
    }

    if (arrayCount < 3) return false; // Need at least [timestamp, title, content]

    // Element 0: timestamp (float64: 0xCB)
    if (mpPos >= len) return false;
    out.timestamp = readFloat64(raw, mpPos, len);

    // Element 1: title (bin)
    if (!readBin(raw, mpPos, len, out.title)) {
        return false;
    }

    // Element 2: content (bin)
    if (!readBin(raw, mpPos, len, out.content)) {
        return false;
    }

    // Element 3: fields (map) — skip it
    if (arrayCount >= 4) {
        if (!skipMsgPackElement(raw, mpPos, len)) {
            // Fields parse failed but we have the core data — soft fail
        }
    }

    // Elements 4+ (e.g., stamp): skip them
    for (size_t i = 4; i < arrayCount; ++i) {
        if (!skipMsgPackElement(raw, mpPos, len)) break;
    }

    return true;
}

// ------------------------------------------------------------------
// verifySignature — check Ed25519 signature on parsed LXMessage
// ------------------------------------------------------------------
bool verifySignature(const LXMessage& msg, const RNS::Identity& sourceIdentity) {
    if (!sourceIdentity) {
        return false;
    }
    if (msg.signature.size() != SIGNATURE_LENGTH) {
        return false;
    }

    try {
        // Reconstruct the MsgPack payload bytes (raw data after headers)
        // We need the original raw bytes. Since we don't have the raw
        // data here, we re-pack the payload from the parsed fields.
        //
        // But wait — signature validation requires the EXACT bytes that
        // were signed. If MsgPack encoding is deterministic (it is for
        // our fixed-format messages), we can reconstruct them.
        //
        // However, to keep things simple and reliable, we use the approach
        // from unpackMessage where we stored the original offset.
        //
        // For now, we reconstruct:
        //   dest_hash + src_hash + msgpack_payload = hashed_part
        //   message_hash = SHA-256(hashed_part)
        //   signed_part = hashed_part + message_hash
        //
        // Then verify: sourceIdentity.validate(signature, signed_part)

        // Rebuild MsgPack payload from parsed fields
        size_t tlen = msg.title.length();
        size_t clen = msg.content.length();

        // Binary sizes (bin8 or bin16)
        size_t titleHdrSize = (tlen <= 255) ? 2 : 3;
        size_t contentHdrSize = (clen <= 255) ? 2 : 3;

        // Total MsgPack payload size
        size_t mpSize = 1                              // fixarray(4)
                      + 9                              // float64
                      + titleHdrSize + tlen            // bin(title)
                      + contentHdrSize + clen          // bin(content)
                      + 1;                             // fixmap(0)

        std::vector<uint8_t> mpData(mpSize);
        size_t pos = 0;

        // fixarray(4)
        mpData[pos++] = 0x94;
        // float64(timestamp)
        writeFloat64(&mpData[pos], msg.timestamp);
        pos += 9;
        // bin(title)
        if (tlen <= 255) {
            writeBin8(&mpData[pos], (const uint8_t*)msg.title.c_str(), (uint8_t)tlen);
            pos += 2 + tlen;
        } else {
            uint16_t len16 = (uint16_t)(tlen > 65535 ? 65535 : tlen);
            writeBin16(&mpData[pos], (const uint8_t*)msg.title.c_str(), len16);
            pos += 3 + len16;
        }
        // bin(content)
        if (clen <= 255) {
            writeBin8(&mpData[pos], (const uint8_t*)msg.content.c_str(), (uint8_t)clen);
            pos += 2 + clen;
        } else {
            uint16_t len16 = (uint16_t)(clen > 65535 ? 65535 : tlen);
            writeBin16(&mpData[pos], (const uint8_t*)msg.content.c_str(), len16);
            pos += 3 + len16;
        }
        // fixmap(0)
        mpData[pos++] = 0x80;

        // Build hashed_part
        RNS::Bytes hashedPart;
        if (msg.destHash.size() > 0) {
            hashedPart.append(msg.destHash);
        } else {
            // OPPORTUNISTIC: dest hash was stripped. We can't validate
            // without the destination hash in the signed data.
            // This is expected — OPPORTUNISTIC messages can't be fully
            // verified without knowing the destination.
            // Create a zero-filled placeholder.
            RNS::Bytes zeroHash(DESTINATION_LENGTH);
            memset(zeroHash.writable(DESTINATION_LENGTH), 0, DESTINATION_LENGTH);
            hashedPart.append(zeroHash);
        }
        hashedPart.append(msg.srcHash);
        hashedPart.append(mpData.data(), mpData.size());

        RNS::Bytes messageHash = RNS::Identity::full_hash(hashedPart);

        RNS::Bytes signedPart;
        signedPart.append(hashedPart);
        signedPart.append(messageHash);

        return sourceIdentity.validate(msg.signature, signedPart);
    }
    catch (const std::exception& e) {
        return false;
    }
    catch (...) {
        return false;
    }
}

// ------------------------------------------------------------------
// encodeAnnounceData — MsgPack([displayName, null, []])
// ------------------------------------------------------------------
RNS::Bytes encodeAnnounceData(const char* displayName) {
    if (!displayName) displayName = "";
    size_t nameLen = strlen(displayName);

    // Calculate total size:
    //   fixarray(3)       = 1 byte
    //   bin(name)          = 2 or 3 + nameLen
    //   nil                = 1 byte
    //   fixarray(0)        = 1 byte
    size_t binHdrSize = (nameLen <= 255) ? 2 : 3;
    size_t totalSize = 1 + binHdrSize + nameLen + 1 + 1;

    std::vector<uint8_t> buf(totalSize);
    size_t pos = 0;

    // fixarray(3)
    buf[pos++] = 0x93;

    // bin(displayName)
    if (nameLen <= 255) {
        writeBin8(&buf[pos], (const uint8_t*)displayName, (uint8_t)nameLen);
        pos += 2 + nameLen;
    } else {
        uint16_t len16 = (uint16_t)(nameLen > 65535 ? 65535 : nameLen);
        writeBin16(&buf[pos], (const uint8_t*)displayName, len16);
        pos += 3 + len16;
    }

    // nil (stamp_cost = null)
    writeNil(&buf[pos]);
    pos += 1;

    // fixarray(0) — empty supported features list
    buf[pos++] = 0x90;

    return RNS::Bytes(buf.data(), buf.size());
}

// ------------------------------------------------------------------
// decodeAnnounceData — extract display name from app_data
// ------------------------------------------------------------------
bool decodeAnnounceData(const RNS::Bytes& appData, char* nameOut, size_t nameOutSize) {
    if (!nameOut || nameOutSize == 0) return false;
    nameOut[0] = '\0';

    if (appData.size() == 0) return false;

    const uint8_t* raw = appData.data();
    size_t len = appData.size();

    // Detect LXMF v0.5.0+ format: first byte is fixarray (0x90-0x9f)
    // or array16 (0xdc) / array32 (0xdd)
    if (raw[0] >= 0x90 && raw[0] <= 0x9F) {
        // LXMF MsgPack format: [displayName, stamp_cost, features]
        uint8_t arrayCount = fixLen(raw[0]);
        if (arrayCount < 1) return false;

        size_t pos = 1;

        // Read first element: bin(displayName)
        std::string dn;
        if (!readBin(raw, pos, len, dn)) {
            // Could also be a str instead of bin
            // Try reading as string
            if (pos >= len) return false;
            uint8_t tag = raw[pos++];
            if (tag >= 0xA0 && tag <= 0xBF) {
                // fixstr
                uint8_t slen = tag & 0x1F;
                if (pos + slen > len) return false;
                dn.assign((const char*)raw + pos, slen);
                pos += slen;
            } else {
                return false;
            }
        }

        if (dn.empty()) return false;

        size_t copyLen = (dn.length() < nameOutSize - 1) ? dn.length() : nameOutSize - 1;
        memcpy(nameOut, dn.c_str(), copyLen);
        nameOut[copyLen] = '\0';
        return true;
    }
    else if (raw[0] == 0xDC || raw[0] == 0xDD) {
        // array16/array32 — more complex, skip for now
        // TODO: handle larger arrays if needed
        return false;
    }
    else {
        // Legacy format: plain UTF-8 string (old ReticuleM instances)
        size_t copyLen = (len < nameOutSize - 1) ? len : nameOutSize - 1;
        memcpy(nameOut, raw, copyLen);
        nameOut[copyLen] = '\0';
        return true;
    }
}

} // namespace LXMFCompat
