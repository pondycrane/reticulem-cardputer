# ReticuleM - Cardputer Messaging Client

A portable **native Reticulum / microReticulum** messaging client for the **M5Stack Cardputer** (ESP32-S3).

Unlike a dumb terminal, ReticuleM runs a full **microReticulum stack** on the device. It connects to your local mesh via WiFi UDP (AutoInterface-compatible), discovers peers via announces, and exchanges end-to-end encrypted messages directly — no Python bridge needed for basic operation.

## Architecture

```
+---------------+      WiFi UDP (broadcast)        +-----------------+
| M5Stack       |  <--- Reticulum packets --->     | Reticulum Node  |
| Cardputer     |      LoRa (868/915 MHz)          | (Pi, laptop,    |
| (microRNS)    |  <--- Reticulum packets --->     |  or other MCU)  |
+---------------+                                    +-----------------+
    |                                                      |
    +--- Cap LoRa-1262 (optional)                          |
    |   SX1262 module on EXT 2.54-14P header               |
    +------------------ LXMF bridge ----------------------+
         (optional: Python bridge relays to LXMF ecosystem)
```

### Key Features

| Feature | Detail |
|---|---|
| **Mesh Stack** | Native microReticulum (Identity, Destination, Packet, Transport, Path Discovery) |
| **Interface** | WiFi UDP broadcast on port 4242 (AutoInterface compatible) + optional LoRa (SX1262) |
| **Crypto** | Ed25519 signatures, X25519 key exchange, AES-128-CBC (handled by microReticulum) |
| **App Protocol** | **LXMF** (`lxmf.delivery`) — MsgPack-encoded LXMessage with Ed25519 signatures (primary) |
|               | **Legacy fallback** (`reticulem.inbox`) — JSON `{v:1, n:name, f:hash, b:body}` (backward compat) |
| **Payload** | Binary LXMF wire format (96-byte header + MsgPack) or legacy JSON |
| **UI** | Splash → Home → Inbox / Compose / Contacts / Settings / Status |
| **Discovery** | Announces every 60s on both `reticulem.inbox` and `lxmf.delivery` aspects; incoming announces auto-populate contacts |
| **Storage** | Identity persisted to SPIFFS; settings JSON; messages in SRAM |

## LoRa Interface

When a **Cap LoRa-1262** (SX1262) module is attached to the CardputerADV's EXT 2.54-14P header, ReticuleM automatically initialises it as a second Reticulum interface alongside WiFi UDP. The two interfaces operate in parallel — packets received over LoRa are routed through the same mesh, and messages can be sent over whichever path is available.

### Pinout (EXT 2.54-14P header to Cap LoRa-1262)

| Signal | GPIO | Notes |
|--------|------|-------|
| SCLK   | 40   | SPI clock |
| MISO   | 39   | SPI master-in, slave-out |
| MOSI   | 14   | SPI master-out, slave-in |
| CS     | 5    | SPI chip-select (active low) |
| DIO1   | 4    | SX1262 IRQ line |
| RST    | 3    | SX1262 reset |
| BUSY   | 6    | SX1262 busy indicator |

### Configuration

The LoRa radio is configured at compile time via PlatformIO build flags in `platformio.ini`:

```ini
-DBOARD_CAP_LORA1262
-DLORA_SCLK=40
-DLORA_MISO=39
-DLORA_MOSI=14
-DLORA_CS=5
-DLORA_DIO1=4
-DLORA_RST=3
-DLORA_BUSY=6
-DLORA_FREQ=868.0
```

- **Frequency:** Set via `LORA_FREQ` (default 868.0 MHz for EU; change to 915.0 for US).
- **Radio parameters:** Bandwidth 125 kHz, SF 8, CR 4/5, TX power 17 dBm (compile-time constants in `lib/CapLoRaInterface/LoRaInterface.h`).
- **Init is non-fatal:** If no module is attached or init fails, the device boots with WiFi only — no crash.

### Status Display

On the **Status** screen you'll see a LoRa line showing:

```
LoRa:  RSSI -112 dBm  SNR 3.2
```

When no packet has been received yet:

```
LoRa:  Online  (no signal yet)
```

When the module is absent or offline:

```
LoRa:  Offline
```

The **status bar** (top-right corner) shows two coloured squares:
- **Left square:** LoRa indicator (green = online + destination active, red = offline)
- **Right square:** WiFi indicator (green = connected, red = disconnected)

### Split-Packet Protocol

LoRa frames are limited to 254 bytes of payload (255 bytes minus 1 header byte). Messages larger than this are automatically split into two frames with a matching sequence number and reassembled on the receiving side.

## Quick Start

### Build & Flash

```bash
cd reticulem-cardputer
export PATH="$HOME/.platformio-venv/bin:$PATH"

# Build
pio run

# Upload to Cardputer (connected at /dev/ttyACM0)
pio run -t upload

# Monitor serial output
pio device monitor
```

### First Boot

1. **WiFi:** On first boot the device tries to connect to SSID `Reticulum-AP` (configurable in `src/App.h` or later in **Settings**).
2. **Identity:** A fresh Reticulum identity is generated and saved to `/identity.txt` on SPIFFS.
3. **Announce:** The Cardputer announces its `reticulem.inbox` destination on the local UDP broadcast domain.

### Network Requirements

Your local Reticulum node (e.g. the Pi) must have **AutoInterface enabled** and optionally **Transport enabled** so it can route for the Cardputer:

```ini
[interfaces]
[[Default Interface]]
  type = AutoInterface
  enabled = Yes
```

The Cardputer broadcasts to `255.255.255.255:4242` (UDP). The Pi's `AutoInterface` will pick this up automatically on the same LAN.

### Messaging

| Screen | Controls |
|---|---|
| **Home** | `8`/`2` navigate, `Enter` select, `Fn+C` quick-compose |
| **Inbox** | `8`/`2` scroll, `Enter` read, `Del` delete, `Fn+Q` back |
| **Compose** | `Tab` switch fields, `Fn+Enter` send, `Fn+Q` back |
| **Contacts** | `8`/`2` scroll, `Enter` select as recipient |
| **Settings** | Edit SSID/Password/Display Name, toggle Transport node, Save |

**Sending to a contact:**
- In **Compose**, type the truncated destination hash (e.g. `a1b2c3d4...`) into the `To:` field, or select from **Contacts**.
- If the path is unknown, ReticuleM initiates path discovery; the message is queued and sent automatically once the announce arrives.
- The recipient must also be running a `reticulem.inbox` destination.

### Receiving Messages

Incoming packets decrypted by microReticulum are parsed and added to the **Inbox**. The sender's hash and display name are auto-added to **Contacts** if new.

## Sideband / LXMF Compatibility

ReticuleM now supports the **LXMF (Lightweight eXtensible Message Format)**
wire protocol natively, making it directly compatible with **Sideband**
(phone/desktop LXMF messaging app by Mark Qvist / unsigned.io).

### How It Works

1. **Dual aspect announces**: On boot, ReticuleM announces both the legacy
   `reticulem.inbox` aspect (plain-text app_data) and the LXMF `lxmf.delivery`
   aspect (MsgPack-encoded app_data with display name, null stamp cost, and
   empty features list).

2. **LXMF-first receiving**: Incoming packets are parsed as LXMF wire format
   first. If parsing succeeds, the message is added to the inbox with Ed25519
   signature verification attempted (if the source identity is known). If LXMF
   parsing fails, the legacy JSON format is tried as a fallback.

3. **OPPORTUNISTIC delivery**: Outgoing messages use LXMF OPPORTUNISTIC
   delivery — the destination hash is stripped from the wire format (Reticulum
   already routes by it), and the packet is sent to the recipient's
   `lxmf.delivery` destination.

4. **Signature verification**: Incoming LXMF messages are verified against
   the sender's Ed25519 public key (stored in contacts). Verification is
   best-effort — messages from unknown identities are accepted without
   signature validation. OPPORTUNISTIC messages (no destination hash) cannot
   be fully verified (known design limitation).

### Wire Format

LXMF messages use a compact binary format over Reticulum packets:

```
┌─────────────────┬─────────────────┬──────────────────┬──────────────┐
│ Destination Hash│ Source Hash     │ Ed25519 Signature│ MsgPack      │
│ (16 bytes)      │ (16 bytes)      │ (64 bytes)       │ Payload      │
│ (stripped for   │                 │                  │              │
│  OPPORTUNISTIC) │                 │                  │              │
└─────────────────┴─────────────────┴──────────────────┴──────────────┘
```

The MsgPack payload is a 4-element array:
```
[
  timestamp: float64,    // Unix time or uptime seconds
  title: bin,            // UTF-8 message title (may be empty)
  content: bin,          // UTF-8 message body
  fields: map            // LXMF fields (ignored on receive, empty on send)
]
```

### LXMF Compatibility Module

The `src/LXMFCompat.h` and `src/LXMFCompat.cpp` files implement the
LXMF wire format in a self-contained namespace `LXMFCompat`. Key design
decisions:

- **Manual MsgPack parser**: A byte-level parser handles exactly the subset
  of MsgPack that LXMF uses (fixarray, float64, bin8/bin16, fixmap, nil),
  avoiding the template-heavy `MsgPack::Unpacker` on the embedded target.
- **Signature reconstruction**: Rather than storing raw byte offsets, MsgPack
  payloads are deterministically rebuilt from parsed fields for Ed25519
  verification.
- **Delivery format detection**: A heuristic based on the offset of the
  MsgPack fixarray marker differentiates DIRECT from OPPORTUNISTIC delivery
  formats.

**API Summary:**

| Function | Purpose |
|----------|---------|
| `packMessage()` | Build LXMF wire format bytes from source identity, destination, title, content |
| `unpackMessage()` | Parse incoming bytes into `LXMessage` struct (auto-detects DIRECT/OPPORTUNISTIC) |
| `verifySignature()` | Validate Ed25519 signature against a recalled identity |
| `encodeAnnounceData()` | Encode display name as MsgPack `[name, null, []]` for LXMF announces |
| `decodeAnnounceData()` | Extract display name from app_data (supports LXMF MsgPack and legacy UTF-8) |

## Development Notes

### End-to-End Testing Rule

Whenever the Cardputer is connected (e.g. at `/dev/ttyACM0`), code changes **must be flashed and verified on the device** before merging. This is not optional — Reticulum is a networked protocol, and emulators can't reproduce timing, WiFi, or mesh behavior.

#### Manual verification

```bash
# Build, flash, and verify boot output
pio run -t upload
pio device monitor
# Confirm: WiFi connects, Reticulum starts, Inbox destination is announced
```

#### Automated hardware smoke test

A PlatformIO unit test environment (`cardputer_test`) is available to flash the device and run an on-device boot assertion:

```bash
# Identify your Cardputer serial port (typically /dev/ttyACM0 on Linux)
pio test --environment cardputer_test --upload-port /dev/ttyACM0
```

This test:
1. Builds the firmware with the same flags as the main `cardputer` environment
2. Flashes it to the device
3. Waits for boot and runs a minimal assertion that the firmware is alive
4. Reports pass/fail via serial

Add this command to your pre-merge checklist. Any code change should pass the smoke test before creating a PR.

### microReticulum Integration

- **Library:** `microReticulum` is loaded as a PlatformIO library from the local clone at `~/microReticulum`.
- **UDP Interface:** A custom `CardputerUDPInterface` (based on the microReticulum example) handles WiFi + UDP broadcast. It skips redundant `WiFi.begin()` if already connected.
- **Filesystem:** `microStore::UniversalFileSystem` is used for Reticulum persistence (path tables, identities). SPIFFS is used independently for app settings and our identity hex key.
- **Memory:** PSRAM pool allocator is configured for microReticulum with a 1MB TLSF pool on the 8MB ESP32-S3.

### Limitations

- **LXMF scope limits.** LXMF support is limited to DIRECT and OPPORTUNISTIC delivery methods (as used by Sideband). Stamps/proof-of-work, propagation nodes, fields (attachments, images, audio), and QR/paper delivery are not implemented. See PR #5 scope for details.
- **RAM messages.** Messages are stored in SRAM and lost on reboot. SPIFFS-based message persistence is a future enhancement.
- **Path discovery timeout.** Sending to an unknown destination requires an announce from that node within ~30s, or the send fails.

## License

MIT — built on M5Stack, microReticulum, and Reticulum.
