// LXMF Protocol Compatibility Tests
//
// Tests the LXMFCompat layer: pack/unpack roundtrip, signature validation,
// announce data encoding/decoding, and legacy format backward compatibility.
//
// Run with:
//   pio test --environment cardputer_test --upload-port /dev/ttyACM0
//
// Requirements:
//   - Cardputer connected via USB

#include <Arduino.h>
#include <unity.h>
#include <microReticulum.h>
#include <MsgPack.h>
#include "../src/LXMFCompat.h"

// Test identity — created with keys for signing
static RNS::Identity g_testIdentity;

// ------------------------------------------------------------------
// Setup: create a test identity with keys
// ------------------------------------------------------------------
void setUp() {
    if (!g_testIdentity) {
        g_testIdentity = RNS::Identity(); // generates new Ed25519 keypair
    }
}

// ------------------------------------------------------------------
// Test: packMessage and unpackMessage roundtrip
// ------------------------------------------------------------------
void test_pack_unpack_roundtrip() {
    const char* title = "Test Title";
    const char* content = "Hello, LXMF world! This is a test message.";
    
    RNS::Bytes destHash = RNS::Identity::get_random_hash(); // random 16-byte hash
    
    // Pack the message
    RNS::Bytes packed = LXMFCompat::packMessage(g_testIdentity, destHash, title, content);
    TEST_ASSERT_TRUE(packed.size() > 0);
    
    // Unpack the message — note: OPPORTUNISTIC delivery strips dest hash,
    // so we simulate that by sending only the tail portion (what the receiver gets)
    RNS::Bytes oppPayload = packed.mid(LXMFCompat::DESTINATION_LENGTH);
    
    LXMFCompat::LXMessage msg;
    bool ok = LXMFCompat::unpackMessage(oppPayload, msg);
    TEST_ASSERT_TRUE(ok);
    
    // Verify parsed fields
    TEST_ASSERT_EQUAL_STRING(title, msg.title.c_str());
    TEST_ASSERT_EQUAL_STRING(content, msg.content.c_str());
    TEST_ASSERT_TRUE(msg.timestamp > 0.0);
    
    // Verify source hash matches our identity
    std::string expectedSrcHex = g_testIdentity.hash().toHex();
    std::string actualSrcHex = msg.srcHash.toHex();
    TEST_ASSERT_EQUAL_STRING(expectedSrcHex.c_str(), actualSrcHex.c_str());
}

// ------------------------------------------------------------------
// Test: packMessage with empty title
// ------------------------------------------------------------------
void test_pack_empty_title() {
    const char* content = "Message with no title";
    RNS::Bytes destHash = RNS::Identity::get_random_hash();
    
    RNS::Bytes packed = LXMFCompat::packMessage(g_testIdentity, destHash, "", content);
    TEST_ASSERT_TRUE(packed.size() > 0);
    
    RNS::Bytes oppPayload = packed.mid(LXMFCompat::DESTINATION_LENGTH);
    LXMFCompat::LXMessage msg;
    bool ok = LXMFCompat::unpackMessage(oppPayload, msg);
    TEST_ASSERT_TRUE(ok);
    
    TEST_ASSERT_EQUAL_STRING("", msg.title.c_str());
    TEST_ASSERT_EQUAL_STRING(content, msg.content.c_str());
}

// ------------------------------------------------------------------
// Test: packMessage with empty content
// ------------------------------------------------------------------
void test_pack_empty_content() {
    const char* title = "Empty body message";
    RNS::Bytes destHash = RNS::Identity::get_random_hash();
    
    RNS::Bytes packed = LXMFCompat::packMessage(g_testIdentity, destHash, title, "");
    TEST_ASSERT_TRUE(packed.size() > 0);
    
    RNS::Bytes oppPayload = packed.mid(LXMFCompat::DESTINATION_LENGTH);
    LXMFCompat::LXMessage msg;
    bool ok = LXMFCompat::unpackMessage(oppPayload, msg);
    TEST_ASSERT_TRUE(ok);
    
    TEST_ASSERT_EQUAL_STRING(title, msg.title.c_str());
    TEST_ASSERT_EQUAL_STRING("", msg.content.c_str());
}

// ------------------------------------------------------------------
// Test: signature validation (valid signature)
// ------------------------------------------------------------------
void test_signature_validation_valid() {
    const char* content = "Message with valid signature";
    RNS::Bytes destHash = RNS::Identity::get_random_hash();
    
    // Pack with source identity (which has the private key)
    RNS::Bytes packed = LXMFCompat::packMessage(g_testIdentity, destHash, "Test", content);
    
    // Unpack the full packed data (simulating DIRECT delivery where dest hash is included)
    LXMFCompat::LXMessage msg;
    bool ok = LXMFCompat::unpackMessage(packed, msg);
    TEST_ASSERT_TRUE(ok);
    
    // Verify signature using the same identity (which has the public key)
    bool sigOk = LXMFCompat::verifySignature(msg, g_testIdentity);
    TEST_ASSERT_TRUE(sigOk);
}

// ------------------------------------------------------------------
// Test: signature validation with tampered content (should fail)
// ------------------------------------------------------------------
void test_signature_validation_tampered() {
    const char* content = "Original content";
    RNS::Bytes destHash = RNS::Identity::get_random_hash();
    
    // Pack
    RNS::Bytes packed = LXMFCompat::packMessage(g_testIdentity, destHash, "Test", content);
    
    // Unpack
    LXMFCompat::LXMessage msg;
    bool ok = LXMFCompat::unpackMessage(packed, msg);
    TEST_ASSERT_TRUE(ok);
    
    // Tamper with the content
    msg.content = "TAMPERED content!!!";
    
    // Signature should fail because content changed
    bool sigOk = LXMFCompat::verifySignature(msg, g_testIdentity);
    TEST_ASSERT_FALSE(sigOk);
}

// ------------------------------------------------------------------
// Test: signature validation with wrong identity (should fail)
// ------------------------------------------------------------------
void test_signature_validation_wrong_identity() {
    const char* content = "Message from A";
    RNS::Bytes destHash = RNS::Identity::get_random_hash();
    
    // Pack with test identity
    RNS::Bytes packed = LXMFCompat::packMessage(g_testIdentity, destHash, "Test", content);
    
    // Unpack
    LXMFCompat::LXMessage msg;
    bool ok = LXMFCompat::unpackMessage(packed, msg);
    TEST_ASSERT_TRUE(ok);
    
    // Create a DIFFERENT identity and try to verify
    RNS::Identity wrongIdentity = RNS::Identity(); // new keys
    
    bool sigOk = LXMFCompat::verifySignature(msg, wrongIdentity);
    TEST_ASSERT_FALSE(sigOk);
}

// ------------------------------------------------------------------
// Test: encodeAnnounceData and decodeAnnounceData roundtrip
// ------------------------------------------------------------------
void test_announce_data_roundtrip() {
    const char* displayName = "Cardputer";
    
    RNS::Bytes appData = LXMFCompat::encodeAnnounceData(displayName);
    TEST_ASSERT_TRUE(appData.size() > 0);
    
    // Verify it starts with a fixarray marker (0x93 for array of 3)
    TEST_ASSERT_EQUAL(0x93, appData.data()[0]);
    
    char decoded[64];
    bool ok = LXMFCompat::decodeAnnounceData(appData, decoded, sizeof(decoded));
    TEST_ASSERT_TRUE(ok);
    TEST_ASSERT_EQUAL_STRING(displayName, decoded);
}

// ------------------------------------------------------------------
// Test: decodeAnnounceData with legacy plain-UTF-8 format
// ------------------------------------------------------------------
void test_announce_data_legacy_format() {
    // Legacy format: just plain UTF-8 bytes, no MsgPack wrapper
    const char* name = "OldReticuleM";
    RNS::Bytes legacyData(name);
    
    char decoded[64];
    bool ok = LXMFCompat::decodeAnnounceData(legacyData, decoded, sizeof(decoded));
    TEST_ASSERT_TRUE(ok);
    TEST_ASSERT_EQUAL_STRING(name, decoded);
}

// ------------------------------------------------------------------
// Test: decodeAnnounceData with empty data
// ------------------------------------------------------------------
void test_announce_data_empty() {
    RNS::Bytes emptyData;
    char decoded[64] = "unchanged";
    bool ok = LXMFCompat::decodeAnnounceData(emptyData, decoded, sizeof(decoded));
    TEST_ASSERT_FALSE(ok);
    // Buffer should be cleared
    TEST_ASSERT_EQUAL_STRING("", decoded);
}

// ------------------------------------------------------------------
// Test: unpackMessage rejects non-LXMF data
// ------------------------------------------------------------------
void test_unpack_rejects_garbage() {
    // Random bytes that don't match LXMF format
    uint8_t garbage[] = {0x00, 0x01, 0x02, 0x03, 0xFF, 0xFE};
    RNS::Bytes badData(garbage, sizeof(garbage));
    
    LXMFCompat::LXMessage msg;
    bool ok = LXMFCompat::unpackMessage(badData, msg);
    TEST_ASSERT_FALSE(ok);
}

// ------------------------------------------------------------------
// Test: unpackMessage handles DIRECT delivery format (with dest hash)
// ------------------------------------------------------------------
void test_unpack_direct_format() {
    const char* content = "DIRECT delivery test";
    RNS::Bytes destHash = RNS::Identity::get_random_hash();
    
    RNS::Bytes packed = LXMFCompat::packMessage(g_testIdentity, destHash, "Direct", content);
    // Full packed data includes dest hash (96-byte header + payload)
    TEST_ASSERT_TRUE(packed.size() > LXMFCompat::DESTINATION_LENGTH * 2 + LXMFCompat::SIGNATURE_LENGTH);
    
    LXMFCompat::LXMessage msg;
    bool ok = LXMFCompat::unpackMessage(packed, msg);
    TEST_ASSERT_TRUE(ok);
    TEST_ASSERT_EQUAL_STRING(content, msg.content.c_str());
}

// ------------------------------------------------------------------
// Test: unpackMessage handles OPPORTUNISTIC delivery format (no dest hash)
// ------------------------------------------------------------------
void test_unpack_opportunistic_format() {
    const char* content = "OPPORTUNISTIC delivery test";
    RNS::Bytes destHash = RNS::Identity::get_random_hash();
    
    RNS::Bytes packed = LXMFCompat::packMessage(g_testIdentity, destHash, "Opp", content);
    // Strip dest hash to simulate OPPORTUNISTIC delivery
    RNS::Bytes oppPayload = packed.mid(LXMFCompat::DESTINATION_LENGTH);
    
    LXMFCompat::LXMessage msg;
    bool ok = LXMFCompat::unpackMessage(oppPayload, msg);
    TEST_ASSERT_TRUE(ok);
    TEST_ASSERT_EQUAL_STRING(content, msg.content.c_str());
}

// ------------------------------------------------------------------
// Test: UTF-8 content roundtrip
// ------------------------------------------------------------------
void test_utf8_content_roundtrip() {
    // UTF-8 test string with emoji and accented characters
    const char* content = u8"Hello 🌍 — café résumé 你好";
    RNS::Bytes destHash = RNS::Identity::get_random_hash();
    
    RNS::Bytes packed = LXMFCompat::packMessage(g_testIdentity, destHash, "UTF-8 Test", content);
    RNS::Bytes oppPayload = packed.mid(LXMFCompat::DESTINATION_LENGTH);
    
    LXMFCompat::LXMessage msg;
    bool ok = LXMFCompat::unpackMessage(oppPayload, msg);
    TEST_ASSERT_TRUE(ok);
    TEST_ASSERT_EQUAL_STRING(content, msg.content.c_str());
}

// ------------------------------------------------------------------
// Test: signature validation with content > 255 bytes (bin16 path)
// Regression test for tlen-vs-clen copy-paste bug in verifySignature
// ------------------------------------------------------------------
void test_signature_validation_long_content() {
    // Generate content > 255 bytes to exercise bin16 path
    std::string content;
    for (int i = 0; i < 280; i++) {
        content += 'A' + (i % 26);
    }
    RNS::Bytes destHash = RNS::Identity::get_random_hash();
    
    // Pack with source identity (which has the private key)
    RNS::Bytes packed = LXMFCompat::packMessage(g_testIdentity, destHash, "LongSig", content.c_str());
    
    // DIRECT format (with dest hash) — this is the format where
    // verifySignature can reconstruct the full hashed_part
    LXMFCompat::LXMessage msg;
    bool ok = LXMFCompat::unpackMessage(packed, msg);
    TEST_ASSERT_TRUE(ok);
    
    // Signature must verify — this catches the tlen/clen bug
    bool sigOk = LXMFCompat::verifySignature(msg, g_testIdentity);
    TEST_ASSERT_TRUE(sigOk);
}

// ------------------------------------------------------------------
// Test: OPPORTUNISTIC signature verification (known limitation)
// Documents that OPPORTUNISTIC messages cannot be verified because
// the dest hash is missing from the payload.
// ------------------------------------------------------------------
void test_signature_validation_opportunistic() {
    const char* content = "OPPORTUNISTIC message";
    RNS::Bytes destHash = RNS::Identity::get_random_hash();
    
    RNS::Bytes packed = LXMFCompat::packMessage(g_testIdentity, destHash, "Test", content);
    // Strip dest hash to simulate OPPORTUNISTIC delivery
    RNS::Bytes oppPayload = packed.mid(LXMFCompat::DESTINATION_LENGTH);
    
    LXMFCompat::LXMessage msg;
    bool ok = LXMFCompat::unpackMessage(oppPayload, msg);
    TEST_ASSERT_TRUE(ok);
    
    // destHash should be empty (OPPORTUNISTIC format)
    TEST_ASSERT_EQUAL(0, msg.destHash.size());
    
    // Signature verification will fail because dest hash is missing
    // (zero-filled placeholder doesn't match original signed data)
    // This is a known limitation — OPPORTUNISTIC messages cannot be
    // fully verified without knowing the destination.
    bool sigOk = LXMFCompat::verifySignature(msg, g_testIdentity);
    TEST_ASSERT_FALSE(sigOk);
}

// ------------------------------------------------------------------
// Test: long content (near 295-byte single-packet limit)
// ------------------------------------------------------------------
void test_long_content_roundtrip() {
    // Generate content near the 295-byte single-packet limit
    std::string content;
    for (int i = 0; i < 280; i++) {
        content += 'A' + (i % 26);
    }
    
    RNS::Bytes destHash = RNS::Identity::get_random_hash();
    
    RNS::Bytes packed = LXMFCompat::packMessage(g_testIdentity, destHash, "Long", content.c_str());
    RNS::Bytes oppPayload = packed.mid(LXMFCompat::DESTINATION_LENGTH);
    
    LXMFCompat::LXMessage msg;
    bool ok = LXMFCompat::unpackMessage(oppPayload, msg);
    TEST_ASSERT_TRUE(ok);
    TEST_ASSERT_EQUAL_STRING(content.c_str(), msg.content.c_str());
}

// ------------------------------------------------------------------
// Test Runner
// ------------------------------------------------------------------
void setup() {
    // Allow serial monitor to attach
    delay(2000);
    
    UNITY_BEGIN();
    
    RUN_TEST(test_pack_unpack_roundtrip);
    RUN_TEST(test_pack_empty_title);
    RUN_TEST(test_pack_empty_content);
    RUN_TEST(test_signature_validation_valid);
    RUN_TEST(test_signature_validation_tampered);
    RUN_TEST(test_signature_validation_wrong_identity);
    RUN_TEST(test_signature_validation_long_content);
    RUN_TEST(test_signature_validation_opportunistic);
    RUN_TEST(test_announce_data_roundtrip);
    RUN_TEST(test_announce_data_legacy_format);
    RUN_TEST(test_announce_data_empty);
    RUN_TEST(test_unpack_rejects_garbage);
    RUN_TEST(test_unpack_direct_format);
    RUN_TEST(test_unpack_opportunistic_format);
    RUN_TEST(test_utf8_content_roundtrip);
    RUN_TEST(test_long_content_roundtrip);
    
    UNITY_END();
}

void loop() {
    delay(100);
}
