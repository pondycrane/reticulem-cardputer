# ReticuleM - Cardputer Messaging Client

A portable **native Reticulum / microReticulum** messaging client for the **M5Stack Cardputer** (ESP32-S3).

Unlike a dumb terminal, ReticuleM runs a full **microReticulum stack** on the device. It connects to your local mesh via WiFi UDP (AutoInterface-compatible), discovers peers via announces, and exchanges end-to-end encrypted messages directly — no Python bridge needed for basic operation.

## Architecture

```
+---------------+      WiFi UDP (broadcast)        +-----------------+
| M5Stack       |  <--- Reticulum packets --->     | Reticulum Node  |
| Cardputer     |                                    | (Pi, laptop,    |
| (microRNS)    |                                    |  or other MCU)  |
+---------------+                                    +-----------------+
         |                                                  |
         +------------------ LXMF bridge ------------------+
         (optional: Python bridge relays to LXMF ecosystem)
```

### Key Features

| Feature | Detail |
|---|---|
| **Mesh Stack** | Native microReticulum (Identity, Destination, Packet, Transport, Path Discovery) |
| **Interface** | WiFi UDP broadcast on port 4242 (AutoInterface compatible) |
| **Crypto** | Ed25519 signatures, X25519 key exchange, AES-128-CBC (handled by microReticulum) |
| **App Protocol** | Custom `reticulem.inbox` over Reticulum Links/Packets |
| **Payload** | Compact JSON: `{v:1, n:name, f:hash, b:body}` |
| **UI** | Splash → Home → Inbox / Compose / Contacts / Settings / Status |
| **Discovery** | Announces every 60s; incoming announces auto-populate contacts |
| **Storage** | Identity persisted to SPIFFS; settings JSON; messages in SRAM |

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

## Optional: LXMF Bridge

If you want to bridge `reticulem.inbox` messages to/from the **LXMF** messaging ecosystem, an optional Python bridge can run on the Pi. It acts as a Reticulum node on the LAN and translates between our lightweight JSON payload and LXMF `LXMessage`.

See `host-bridge/bridge.py` (legacy JSON-over-serial) or adapt it for native Reticulum packet relaying.

## Development Notes

### microReticulum Integration

- **Library:** `microReticulum` is loaded as a PlatformIO library from the local clone at `~/microReticulum`.
- **UDP Interface:** A custom `CardputerUDPInterface` (based on the microReticulum example) handles WiFi + UDP broadcast. It skips redundant `WiFi.begin()` if already connected.
- **Filesystem:** `microStore::UniversalFileSystem` is used for Reticulum persistence (path tables, identities). SPIFFS is used independently for app settings and our identity hex key.
- **Memory:** PSRAM pool allocator is configured for microReticulum with a 1MB TLSF pool on the 8MB ESP32-S3.

### Limitations

- **No LXMF on-device.** LXMF is not yet implemented in microReticulum. ReticuleM uses its own lightweight app protocol. A bridge can map to LXMF if desired.
- **WiFi only.** No LoRa interface is configured in this build, but the Cardputer ADV could be extended with one.
- **RAM messages.** Messages are stored in SRAM and lost on reboot. SPIFFS-based message persistence is a future enhancement.
- **Path discovery timeout.** Sending to an unknown destination requires an announce from that node within ~30s, or the send fails.

## License

MIT — built on M5Stack, microReticulum, and Reticulum.
