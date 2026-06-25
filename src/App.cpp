#include "App.h"
#include <SPIFFS.h>
#include <ArduinoJson.h>
#include <LoRaInterface.h>

// ------------------------------------------------------------------
// Global app pointer for C-style callbacks
// ------------------------------------------------------------------
static ReticuleM* g_app = nullptr;

static void staticPacketCallback(const RNS::Bytes& data, const RNS::Packet& packet) {
    if (g_app) g_app->onPacketReceived(data, packet);
}

// ------------------------------------------------------------------
// ReticuleM Announce Handler subclass
// ------------------------------------------------------------------
class ReticuleMAnnounceHandler : public RNS::AnnounceHandler {
public:
    ReticuleM* app;
    ReticuleMAnnounceHandler(ReticuleM* a) : RNS::AnnounceHandler("reticulem.inbox"), app(a) {}
    virtual void received_announce(const RNS::Bytes& destination_hash,
                                   const RNS::Identity& announced_identity,
                                   const RNS::Bytes& app_data) override {
        if (app) app->onAnnounceReceived(destination_hash, announced_identity, app_data);
    }
};

#ifdef ARDUINO
extern "C" int _write(int file, char *ptr, int len) {
    return Serial.write(ptr, len);
}
#endif

// ------------------------------------------------------------------
// Lifecycle
// ------------------------------------------------------------------
ReticuleM::ReticuleM()
    : state(AppState::SPLASH),
      lastState(AppState::SPLASH),
      stateChanged(true),
      udpInterface(nullptr),
      keyPressed(false),
      lastChar(0),
      fnDown(false),
      ctrlDown(false),
      cursorPos(0),
      messageCount(0),
      messageSelected(0),
      contactCount(0),
      contactSelected(0),
      composeField(0),
      lastBlink(0),
      cursorVisible(true),
      splashStart(0),
      lastAnnounce(0),
      lastStatusUpdate(0),
      pendingSendActive(false),
      pendingSendStart(0),
      wifiConnected(false),
      loraOnline(false),
      loraRSSI(NAN),
      loraSNR(NAN),
      peerCount(0),
      nextMsgId(1),
      homeSelected(0),
      settingsSel(0),
      settingsEditing(false),
      settingsEditTarget(nullptr),
      settingsEditMax(0)
{
    memset(composeTo, 0, sizeof(composeTo));
    memset(composeBody, 0, sizeof(composeBody));
    memset(ssid, 0, sizeof(ssid));
    memset(password, 0, sizeof(password));
    memset(displayName, 0, sizeof(displayName));
    memset(ownHash, 0, sizeof(ownHash));
    memset(nodeStatus, 0, sizeof(nodeStatus));
    memset(loraError, 0, sizeof(loraError));
    memset(pendingSendTo, 0, sizeof(pendingSendTo));
    memset(pendingSendBody, 0, sizeof(pendingSendBody));
    
    // Defaults — overridden by SPIFFS settings if available
    strncpy(displayName, "Cardputer", sizeof(displayName)-1);
    // WiFi credentials are loaded from /settings.json on SPIFFS.
    // Set to empty to avoid hardcoding secrets in source; first-boot
    // user must configure via the Settings screen.
    ssid[0] = '\0';
    password[0] = '\0';
    transportEnabled = false;
    
    g_app = this;
}

void ReticuleM::begin() {
    Serial.begin(115200);
    delay(100);
    Serial.println("ReticuleM boot with microReticulum");
    
    initDisplay();
    renderSplash();
    initFS();
    initReticulum();
    initWiFi();  // non-blocking — see update()
    
    splashStart = millis();
}

// ------------------------------------------------------------------
// Main Loop
// ------------------------------------------------------------------
void ReticuleM::update() {
    M5Cardputer.update();
    processKeyboard();
    reticulumLoop();

    if (millis() - lastBlink > 500) {
        lastBlink = millis();
        cursorVisible = !cursorVisible;
    }

    checkWiFiConnection();
    
    if (millis() - lastStatusUpdate > 2000) {
        lastStatusUpdate = millis();
        wifiConnected = (WiFi.status() == WL_CONNECTED);
        checkLoRaConnection();
        drawStatusBar();
    }

    stateChanged = (state != lastState);
    lastState = state;
    
    switch (state) {
        case AppState::SPLASH:    runSplash(); break;
        case AppState::HOME:      runHome(); break;
        case AppState::INBOX:     runInbox(); break;
        case AppState::COMPOSE:   runCompose(); break;
        case AppState::CONTACTS:  runContacts(); break;
        case AppState::SETTINGS:  runSettings(); break;
        case AppState::STATUS:    runStatus(); break;
    }
}

// ------------------------------------------------------------------
// Hardware Init
// ------------------------------------------------------------------
void ReticuleM::initDisplay() {
    auto cfg = M5.config();
    M5Cardputer.begin(cfg, true);
    M5Cardputer.Display.setRotation(1);
    M5Cardputer.Display.setBrightness(128);
    M5Cardputer.Display.fillScreen(TFT_BLACK);
}

void ReticuleM::initFS() {
    if (!SPIFFS.begin(true)) {
        Serial.println("SPIFFS init failed");
    } else {
        // Load settings from /settings.json if exists
        File f = SPIFFS.open("/settings.json", "r");
        if (f) {
            StaticJsonDocument<512> doc;
            deserializeJson(doc, f);
            strlcpy(ssid, doc["ssid"] | ssid, sizeof(ssid));
            strlcpy(password, doc["password"] | password, sizeof(password));
            strlcpy(displayName, doc["name"] | displayName, sizeof(displayName));
            transportEnabled = doc["transport"] | false;
            f.close();
        }
        Serial.println("SPIFFS ready");
    }
}

void ReticuleM::initWiFi() {
    if (strlen(ssid) == 0) return;
    WiFi.begin(ssid, password);
    wifiConnectStart = millis();
    Serial.print("Connecting to WiFi (async)...");
}

void ReticuleM::checkLoRaConnection() {
    if (loraImpl) {
        loraOnline = loraImpl->isOnline();
        if (loraOnline) {
            loraRSSI = loraImpl->getRSSI();
            loraSNR  = loraImpl->getSNR();
            loraError[0] = '\0';  // Clear error
        } else {
            loraRSSI = NAN;
            loraSNR = NAN;
            snprintf(loraError, sizeof(loraError), "Init failed");
        }
    } else {
        loraOnline = false;
        loraRSSI = NAN;
        loraSNR = NAN;
        snprintf(loraError, sizeof(loraError), "No module");
    }
}

void ReticuleM::checkWiFiConnection() {
    if (wifiConnected || wifiConnectStart == 0) return;
    
    if (WiFi.status() == WL_CONNECTED) {
        wifiConnected = true;
        wifiConnectStart = 0;
        Serial.println("");
        Serial.print("WiFi IP: ");
        Serial.println(WiFi.localIP());
    } else if (millis() - wifiConnectStart > 20000) {
        Serial.println("");
        Serial.println("WiFi connection failed (timeout)");
        wifiConnectStart = 0;
    }
}

// ------------------------------------------------------------------
// Reticulum Stack
// ------------------------------------------------------------------
void ReticuleM::initReticulum() {
    INFO("Init microReticulum...");
    
    try {
        // Register filesystem for RNS persistence
        microStore::FileSystem fs{microStore::Adapters::NoopFileSystem()};
        fs.init();
        RNS::Utilities::OS::register_filesystem(fs);
        
        // UDP Interface
        udpImpl = std::make_unique<CardputerUDPInterface>("udp0");
        udpImpl->setWiFiCredentials(ssid, password);
        udpImpl->setRemoteHost("255.255.255.255", 4242);
        udpInterface = RNS::Interface(udpImpl.get());
        udpInterface.mode(RNS::Type::Interface::MODE_GATEWAY);
        RNS::Transport::register_interface(udpInterface);
        udpInterface.start();

        // LoRa interface — Cap LoRa-1262 (U214) on EXT 2.54-14P
        // Init failure is non-fatal — degrade gracefully to WiFi-only
        {
            loraImpl = std::make_unique<LoRaInterface>("lora0");
            loraInterface = RNS::Interface(loraImpl.get());
            loraInterface.mode(RNS::Type::Interface::MODE_GATEWAY);
            try {
                if (loraImpl->start()) {
                    RNS::Transport::register_interface(loraInterface);
                    INFO("LoRa interface registered");
                } else {
                    WARNING("LoRa interface start returned false — continuing with WiFi only");
                    loraImpl.reset();
                }
            } catch (const std::exception& e) {
                WARNINGF("LoRa interface exception: %s — continuing with WiFi only", e.what());
                loraImpl.reset();
            }
        }
        
        // Reticulum instance
        reticulum = RNS::Reticulum();
        reticulum.transport_enabled(transportEnabled);
        reticulum.probe_destination_enabled(true);
        reticulum.remote_management_enabled(false);
        reticulum.start();
        
        // Identity
        loadIdentity();
        strlcpy(ownHash, identity.hash().toHex().c_str(), sizeof(ownHash));
        INFOF("Identity loaded, hash: %s", ownHash);
        
        // Inbox destination
        inboxDest = RNS::Destination(
            identity,
            RNS::Type::Destination::IN,
            RNS::Type::Destination::SINGLE,
            "reticulem",
            "inbox"
        );
        inboxDest.set_packet_callback(staticPacketCallback);
        inboxDest.set_proof_strategy(RNS::Type::Destination::PROVE_ALL);
        INFOF("Inbox destination: %s", inboxDest.hash().toHex().c_str());
        
        // Announce handler
        announceHandler = RNS::HAnnounceHandler(new ReticuleMAnnounceHandler(this));
        RNS::Transport::register_announce_handler(announceHandler);
        
        // Initial announce
        performAnnounce();
        strncpy(nodeStatus, "Ready", sizeof(nodeStatus));
    }
    catch (const std::exception& e) {
        ERRORF("Reticulum init exception: %s", e.what());
        strncpy(nodeStatus, "Init error", sizeof(nodeStatus));
    }
}

void ReticuleM::reticulumLoop() {
    try {
        reticulum.loop();
        
        // Periodic announce every 60 seconds
        if (millis() - lastAnnounce > 60000) {
            performAnnounce();
            lastAnnounce = millis();
        }
        
        // Flush pending sends
        if (pendingSendActive) {
            flushPendingSend();
        }
    }
    catch (const std::exception& e) {
        ERRORF("Reticulum loop exception: %s", e.what());
    }
}

void ReticuleM::loadIdentity() {
    File f = SPIFFS.open("/identity.txt", "r");
    if (f) {
        String hex = f.readStringUntil('\n');
        hex.trim();
        f.close();
        // Ed25519 private key is 32 bytes (64 hex chars)
        constexpr size_t KEY_HEX_LEN = 64;
        if (hex.length() >= KEY_HEX_LEN) {
            RNS::Bytes prv;
            prv.assignHex(hex.c_str());
            if (prv.size() == KEY_HEX_LEN / 2) {
                identity = RNS::Identity(false);
                identity.load_private_key(prv);
                INFO("Loaded existing identity");
                return;
            } else {
                WARNING("Saved identity key invalid, generating new");
            }
        }
    }
    // Generate new
    identity = RNS::Identity();
    saveIdentity();
    INFO("Generated new identity");
}

void ReticuleM::saveIdentity() {
    RNS::Bytes prv = identity.get_private_key();
    File f = SPIFFS.open("/identity.txt", "w");
    if (f) {
        f.print(prv.toHex().c_str());
        f.println();
        f.close();
    }
}

void ReticuleM::performAnnounce() {
    if (inboxDest) {
        RNS::Bytes app_data(displayName);
        inboxDest.announce(app_data);
        INFO("Announced reticulem.inbox");
    }
}

// ------------------------------------------------------------------
// Message Receiving
// ------------------------------------------------------------------
void ReticuleM::onPacketReceived(const RNS::Bytes& data, const RNS::Packet& packet) {
    // Parse JSON payload
    String json = data.toString().c_str();
    StaticJsonDocument<512> doc;
    DeserializationError err = deserializeJson(doc, json);
    // Rate-limit bad packet logging to avoid log spam and CPU waste
    static int badPacketCount = 0;
    if (err || !doc["b"].is<const char*>()) {
        if (badPacketCount < 10) {
            INFOF("Bad JSON payload: %s", err ? err.c_str() : "missing body");
        }
        badPacketCount++;
        return;
    }
    badPacketCount = 0;
    
    const char* from = doc["f"] | "unknown";
    const char* name = doc["n"] | "";
    const char* body = doc["b"] | "";
    
    // Add to inbox
    if (messageCount < MAX_MESSAGES) {
        Message m{};
        m.id = nextMsgId++;
        m.timestamp = millis();
        strlcpy(m.sender, from, sizeof(m.sender));
        strlcpy(m.recipient, ownHash, sizeof(m.recipient));
        strlcpy(m.content, body, sizeof(m.content));
        m.outgoing = false;
        m.read = false;
        messages[messageCount++] = m;
    }
    
    // Auto-add/update contact
    if (strlen(name) > 0) {
        bool found = false;
        for (int i = 0; i < contactCount; i++) {
            if (strcmp(contacts[i].hash, from) == 0) {
                strlcpy(contacts[i].name, name, sizeof(contacts[i].name));
                found = true;
                break;
            }
        }
        if (!found && contactCount < MAX_CONTACTS) {
            Contact c{};
            strlcpy(c.name, name, sizeof(c.name));
            strlcpy(c.hash, from, sizeof(c.hash));
            contacts[contactCount++] = c;
        }
    }
    
    INFOF("Received msg from %s", from);
}

void ReticuleM::onAnnounceReceived(const RNS::Bytes& destination_hash,
                                   const RNS::Identity& announced_identity,
                                   const RNS::Bytes& app_data) {
    const char* name = app_data.toString().c_str();
    const char* hash = destination_hash.toHex().c_str();
    // Store the full 32-byte identity hash for Identity::recall()
    RNS::Bytes fullIdHash = announced_identity.hash();
    if (strlen(name) == 0 || strcmp(hash, ownHash) == 0) return;
    
    bool found = false;
    for (int i = 0; i < contactCount; i++) {
        if (strcmp(contacts[i].hash, hash) == 0) {
            strlcpy(contacts[i].name, name, sizeof(contacts[i].name));
            contacts[i].fullHash = fullIdHash;
            found = true;
            break;
        }
    }
    if (!found && contactCount < MAX_CONTACTS) {
        Contact c{};
        strlcpy(c.name, name, sizeof(c.name));
        strlcpy(c.hash, hash, sizeof(c.hash));
        c.fullHash = fullIdHash;
        contacts[contactCount++] = c;
        INFOF("New contact: %s (%s)", name, hash);
    }
}

// ------------------------------------------------------------------
// Message Sending
// ------------------------------------------------------------------
void ReticuleM::sendNativeMessage(const char* recipient_hex, const char* body) {
    // Sanitize hex — strip angle brackets safely
    const char* start = recipient_hex;
    const char* end = recipient_hex + strlen(recipient_hex);
    if (*start == '<') start++;
    if (end > start && *(end-1) == '>') end--;
    size_t len = end - start;
    char hex_clean[128];
    size_t copyLen = (len < sizeof(hex_clean) - 1) ? len : sizeof(hex_clean) - 1;
    memcpy(hex_clean, start, copyLen);
    hex_clean[copyLen] = '\0';
    
    RNS::Bytes hash;
    try {
        hash.assignHex(hex_clean);
    } catch (const std::exception& e) {
        WARNINGF("Invalid hex format: %s", e.what());
        strncpy(nodeStatus, "Bad hash", sizeof(nodeStatus));
        return;
    } catch (...) {
        WARNING("Unknown exception during hex conversion");
        strncpy(nodeStatus, "Bad hash", sizeof(nodeStatus));
        return;
    }
    
    // Add to local sent box
    if (messageCount < MAX_MESSAGES) {
        Message m{};
        m.id = nextMsgId++;
        m.timestamp = millis();
        strlcpy(m.sender, ownHash, sizeof(m.sender));
        strlcpy(m.recipient, hex_clean, sizeof(m.recipient));
        strlcpy(m.content, body, sizeof(m.content));
        m.outgoing = true;
        m.read = true;
        messages[messageCount++] = m;
    }
    
    if (!RNS::Transport::has_path(hash)) {
        RNS::Transport::request_path(hash);
        strlcpy(pendingSendTo, hex_clean, sizeof(pendingSendTo));
        strlcpy(pendingSendBody, body, sizeof(pendingSendBody));
        pendingSendActive = true;
        pendingSendStart = millis();
        strncpy(nodeStatus, "Finding path...", sizeof(nodeStatus));
        INFO("Path discovery started for message");
        return;
    }
    
    doSendNativeMessage(hash, body);
}

void ReticuleM::doSendNativeMessage(const RNS::Bytes& hash, const char* body) {
    // Look up the full identity hash from contacts (truncated hash won't work for recall)
    RNS::Bytes recallHash = hash;
    std::string hexHash = hash.toHex();
    for (int i = 0; i < contactCount; i++) {
        if (hexHash == std::string(contacts[i].hash)) {
            if (contacts[i].fullHash.size() > 0) {
                recallHash = contacts[i].fullHash;
            }
            break;
        }
    }
    
    RNS::Identity recipient_identity = RNS::Identity::recall(recallHash);
    if (!recipient_identity) {
        strncpy(nodeStatus, "Identity recall failed", sizeof(nodeStatus));
        WARNING("Could not recall identity for destination");
        return;
    }
    
    RNS::Destination recipient(
        recipient_identity,
        RNS::Type::Destination::OUT,
        RNS::Type::Destination::SINGLE,
        "reticulem",
        "inbox"
    );
    
    StaticJsonDocument<512> doc;
    doc["v"] = 1;
    doc["n"] = displayName;
    doc["f"] = ownHash;
    doc["b"] = body;
    String json;
    serializeJson(doc, json);
    
    RNS::Packet packet(recipient, RNS::Bytes(json.c_str(), json.length()));
    RNS::PacketReceipt receipt = packet.receipt_send();
    if (receipt) {
        strncpy(nodeStatus, "Sent", sizeof(nodeStatus));
        INFO("Message sent");
    } else {
        strncpy(nodeStatus, "Send failed", sizeof(nodeStatus));
        WARNING("Message send failed");
    }
}

void ReticuleM::flushPendingSend() {
    if (!pendingSendActive) return;
    
    RNS::Bytes hash;
    try {
        hash.assignHex(pendingSendTo);
    } catch (const std::exception& e) {
        WARNINGF("Invalid pending hex format: %s", e.what());
        pendingSendActive = false;
        strncpy(nodeStatus, "Bad pending hash", sizeof(nodeStatus));
        return;
    } catch (...) {
        WARNING("Unknown exception during pending hex conversion");
        pendingSendActive = false;
        strncpy(nodeStatus, "Bad pending hash", sizeof(nodeStatus));
        return;
    }
    
    if (RNS::Transport::has_path(hash)) {
        doSendNativeMessage(hash, pendingSendBody);
        pendingSendActive = false;
    } else if (millis() - pendingSendStart > 30000) {
        pendingSendActive = false;
        strncpy(nodeStatus, "Path timeout", sizeof(nodeStatus));
        WARNING("Path discovery timed out for pending message");
    }
}

// ------------------------------------------------------------------
// Splash Screen
// ------------------------------------------------------------------
void ReticuleM::renderSplash() {
    clearScreen(TFT_BLACK);
    M5Cardputer.Display.setTextColor(TFT_GREEN);
    M5Cardputer.Display.setTextDatum(middle_center);
    M5Cardputer.Display.setTextFont(&fonts::FreeMonoBold12pt7b);
    M5Cardputer.Display.drawString("ReticuleM",
        SCREEN_W / 2, SCREEN_H / 2 - 20);
    M5Cardputer.Display.setTextFont(&fonts::FreeMono9pt7b);
    M5Cardputer.Display.setTextColor(TFT_LIGHTGREY);
    M5Cardputer.Display.drawString("microReticulum",
        SCREEN_W / 2, SCREEN_H / 2 + 8);
    M5Cardputer.Display.drawString("v0.2",
        SCREEN_W / 2, SCREEN_H / 2 + 24);
}

void ReticuleM::runSplash() {
    if (millis() - splashStart > 2000) {
        state = AppState::HOME;
        clearScreen();
    }
}

// ------------------------------------------------------------------
// Home
// ------------------------------------------------------------------
void ReticuleM::runHome() {
    static bool dirty = true;
    const char* items[] = {"Inbox", "Compose", "Contacts", "Settings", "Status"};
    const int count = 5;

    if (dirty) {
        clearScreen();
        drawHeader("ReticuleM");
        drawMenu(items, count, homeSelected);
        drawFooter("^v:nav  Enter:open  Fn+C:compose");
        dirty = false;
    }

    if (keyPressed) {
        if (lastChar == '2' || lastChar == KEY_DOWN) {
            homeSelected = (homeSelected + 1) % count;
            dirty = true;
        } else if (lastChar == '8' || lastChar == KEY_UP) {
            homeSelected = (homeSelected - 1 + count) % count;
            dirty = true;
        } else if (lastChar == KEY_ENTER) {
            switch (homeSelected) {
                case 0: state = AppState::INBOX; messageSelected = 0; dirty = true; break;
                case 1: state = AppState::COMPOSE; composeField = 0;
                        memset(composeTo, 0, sizeof(composeTo));
                        memset(composeBody, 0, sizeof(composeBody));
                        dirty = true; break;
                case 2: state = AppState::CONTACTS; contactSelected = 0; dirty = true; break;
                case 3: state = AppState::SETTINGS; dirty = true; break;
                case 4: state = AppState::STATUS; dirty = true; break;
            }
            return;
        } else if (fnDown && lastChar == 'c') {
            state = AppState::COMPOSE;
            composeField = 0;
            memset(composeTo, 0, sizeof(composeTo));
            memset(composeBody, 0, sizeof(composeBody));
            dirty = true;
            return;
        }
    }
}

// ------------------------------------------------------------------
// Inbox
// ------------------------------------------------------------------
void ReticuleM::runInbox() {
    bool dirty = stateChanged;

    if (dirty) {
        clearScreen();
        drawHeader("Inbox");
        drawMessageList();
        drawFooter("\u2191\u2193:scroll  Enter:read  Del:drop  `=Esc");
        dirty = false;
    }

    if (keyPressed) {
        if (lastChar == '2' || lastChar == KEY_DOWN) {
            if (messageSelected < messageCount - 1) { messageSelected++; dirty = true; }
        } else if (lastChar == '8' || lastChar == KEY_UP) {
            if (messageSelected > 0) { messageSelected--; dirty = true; }
        } else if (lastChar == KEY_ENTER) {
            if (messageCount > 0 && messageSelected < messageCount) {
                messages[messageSelected].read = true;
                clearScreen();
                drawHeader("Message");
                M5Cardputer.Display.setTextFont(&fonts::FreeMono9pt7b);
                M5Cardputer.Display.setTextDatum(top_left);
                M5Cardputer.Display.setTextColor(TFT_WHITE);
                int y = 16;
                M5Cardputer.Display.drawString(String("From: ") + messages[messageSelected].sender, 2, y);
                y += 12;
                M5Cardputer.Display.drawString(String("To: ") + messages[messageSelected].recipient, 2, y);
                y += 16;
                String body = messages[messageSelected].content;
                std::vector<String> lines;
                wrapText(body, SCREEN_W - 4, lines);
                for (const String& line : lines) {
                    if (y >= SCREEN_H - 16) break;
                    M5Cardputer.Display.drawString(line, 2, y);
                    y += 12;
                }
                drawFooter("Any key to return");
                // Wait for any key to return
                dirty = true;
                keyPressed = false;
            }
        } else if (lastChar == KEY_BACKSPACE || lastChar == KEY_DEL) {
            if (messageCount > 0 && messageSelected < messageCount) {
                for (int i = messageSelected; i < messageCount - 1; i++) {
                    messages[i] = messages[i + 1];
                }
                messageCount--;
                if (messageSelected >= messageCount && messageSelected > 0) messageSelected--;
                dirty = true;
            }
        } else if (lastChar == KEY_ESC) {
            state = AppState::HOME;
            dirty = true;
            return;
        } else {
            // Returning from detail view
            dirty = true;
            keyPressed = false;
            return;
        }
    }
}

// ------------------------------------------------------------------
// Compose
// ------------------------------------------------------------------
void ReticuleM::runCompose() {
    bool dirty = stateChanged;

    if (dirty) {
        clearScreen();
        drawHeader("Compose");
        M5Cardputer.Display.setTextFont(&fonts::FreeMono9pt7b);
        M5Cardputer.Display.setTextDatum(top_left);

        int y = 16;
        M5Cardputer.Display.setTextColor(composeField == 0 ? TFT_CYAN : TFT_DARKGREY);
        M5Cardputer.Display.drawString("To:", 2, y);
        M5Cardputer.Display.setTextColor(TFT_WHITE);
        M5Cardputer.Display.drawString(String(composeTo) + (composeField == 0 && cursorVisible ? "_" : ""), 28, y);
        y += 14;

        M5Cardputer.Display.setTextColor(composeField == 1 ? TFT_CYAN : TFT_DARKGREY);
        M5Cardputer.Display.drawString("Msg:", 2, y);
        M5Cardputer.Display.setTextColor(TFT_WHITE);
        String bodyPreview = String(composeBody);
        if (composeField == 1 && cursorVisible) bodyPreview += "_";
        std::vector<String> lines;
        wrapText(bodyPreview, SCREEN_W - 36, lines);
        for (const String& line : lines) {
            if (y >= SCREEN_H - 14) break;
            M5Cardputer.Display.drawString(line, 36, y);
            y += 12;
        }

        // Footer with character count
        char footerBuf[80];
        int bodyLen = strlen(composeBody);
        int maxBody = (int)sizeof(composeBody) - 1;
        snprintf(footerBuf, sizeof(footerBuf),
            "Tab:field  %d/%d  Fn+Enter:send  `=Esc", bodyLen, maxBody);
        drawFooter(footerBuf);
        dirty = false;
    }

    if (keyPressed) {
        if (lastChar == KEY_ESC) {
            state = AppState::HOME;
            dirty = true;
            return;
        }

        if (fnDown && lastChar == KEY_ENTER) {
            if (strlen(composeTo) > 0 && strlen(composeBody) > 0) {
                sendNativeMessage(composeTo, composeBody);
            }
            state = AppState::INBOX;
            dirty = true;
            return;
        }

        if (lastChar == KEY_TAB) {
            composeField = (composeField + 1) % 2;
            dirty = true;
            return;
        }

        if (lastChar == KEY_BACKSPACE || lastChar == KEY_DEL) {
            if (composeField == 0 && strlen(composeTo) > 0) {
                composeTo[strlen(composeTo) - 1] = '\0';
            } else if (composeField == 1 && strlen(composeBody) > 0) {
                composeBody[strlen(composeBody) - 1] = '\0';
            }
            dirty = true;
        } else if (lastChar >= 32 && lastChar < 127) {
            if (composeField == 0 && strlen(composeTo) < sizeof(composeTo) - 1) {
                size_t len = strlen(composeTo);
                composeTo[len] = lastChar;
                composeTo[len + 1] = '\0';
            } else if (composeField == 1 && strlen(composeBody) < sizeof(composeBody) - 1) {
                size_t len = strlen(composeBody);
                composeBody[len] = lastChar;
                composeBody[len + 1] = '\0';
            }
            dirty = true;
        }
    }
}

// ------------------------------------------------------------------
// Contacts
// ------------------------------------------------------------------
void ReticuleM::runContacts() {
    bool dirty = stateChanged;

    if (dirty) {
        clearScreen();
        drawHeader("Contacts");
        if (contactCount == 0) {
            M5Cardputer.Display.setTextFont(&fonts::FreeMono9pt7b);
            M5Cardputer.Display.setTextDatum(middle_center);
            M5Cardputer.Display.setTextColor(TFT_DARKGREY);
            M5Cardputer.Display.drawString("No contacts", SCREEN_W / 2, SCREEN_H / 2);
        } else {
            const int lineHeight = 13;
            int start = max(0, contactSelected - 8);
            for (int i = start; i < contactCount && (i - start) < 9; i++) {
                int y = 16 + (i - start) * lineHeight;
                if (i == contactSelected) {
                    M5Cardputer.Display.fillRect(0, y, SCREEN_W, lineHeight, TFT_DARKCYAN);
                    M5Cardputer.Display.setTextColor(TFT_BLACK);
                } else {
                    M5Cardputer.Display.setTextColor(TFT_WHITE);
                }
                M5Cardputer.Display.setTextFont(&fonts::FreeMono9pt7b);
                M5Cardputer.Display.setTextDatum(top_left);
                char lineBuf[48];
                snprintf(lineBuf, sizeof(lineBuf), "%s %.12s...", contacts[i].name, contacts[i].hash);
                M5Cardputer.Display.drawString(lineBuf, 2, y);
            }
        }
        drawFooter("\u2191\u2193:scroll  Enter:use  `=Esc");
        dirty = false;
    }

    if (keyPressed) {
        if (lastChar == KEY_ESC) {
            state = AppState::HOME;
            dirty = true;
            return;
        }
        if (lastChar == '2' || lastChar == KEY_DOWN) {
            if (contactSelected < contactCount - 1) { contactSelected++; dirty = true; }
        } else if (lastChar == '8' || lastChar == KEY_UP) {
            if (contactSelected > 0) { contactSelected--; dirty = true; }
        } else if (lastChar == KEY_ENTER) {
            if (contactCount > 0 && contactSelected < contactCount) {
                strncpy(composeTo, contacts[contactSelected].hash, sizeof(composeTo) - 1);
                state = AppState::COMPOSE;
                composeField = 1;
                dirty = true;
            }
        }
    }
}

// ------------------------------------------------------------------
// Settings
// ------------------------------------------------------------------
void ReticuleM::runSettings() {
    bool dirty = stateChanged;
    const char* labels[] = {"WiFi SSID", "WiFi Pass", "Display Name", "Transport Node", "Save"};
    const int scount = 5;

    if (dirty) {
        clearScreen();
        drawHeader("Settings");
        M5Cardputer.Display.setTextFont(&fonts::FreeMono9pt7b);
        M5Cardputer.Display.setTextDatum(top_left);

        for (int i = 0; i < scount; i++) {
            int y = 16 + i * 14;
            bool active = (i == settingsSel);
            if (active && settingsEditing) {
                M5Cardputer.Display.fillRect(0, y, SCREEN_W, 14, TFT_DARKGREEN);
                M5Cardputer.Display.setTextColor(TFT_BLACK);
            } else if (active) {
                M5Cardputer.Display.fillRect(0, y, SCREEN_W, 14, TFT_DARKGREY);
                M5Cardputer.Display.setTextColor(TFT_WHITE);
            } else {
                M5Cardputer.Display.setTextColor(TFT_LIGHTGREY);
            }

            const char* val = "";
            switch (i) {
                case 0: val = ssid; break;
                case 1: val = password; break;
                case 2: val = displayName; break;
                case 3: val = transportEnabled ? "Yes" : "No"; break;
                case 4: val = "Write to flash"; break;
            }
            String line = String(labels[i]) + ": " + val + ((active && settingsEditing && cursorVisible) ? "_" : "");
            M5Cardputer.Display.drawString(line, 2, y);
        }

        if (!settingsEditing) drawFooter("^v:scroll  Enter:edit  `=Esc");
        else drawFooter("Enter:done  Del:clr");
        dirty = false;
    }

    if (keyPressed) {
        if (lastChar == KEY_ESC) {
            state = AppState::HOME;
            dirty = true;
            return;
        }

        if (!settingsEditing) {
            if (lastChar == '2' || lastChar == KEY_DOWN) {
                settingsSel = (settingsSel + 1) % scount;
                dirty = true;
            } else if (lastChar == '8' || lastChar == KEY_UP) {
                settingsSel = (settingsSel - 1 + scount) % scount;
                dirty = true;
            } else if (lastChar == KEY_ENTER) {
                if (settingsSel == 3) {
                    transportEnabled = !transportEnabled;
                    reticulum.transport_enabled(transportEnabled);
                    dirty = true;
                } else if (settingsSel == 4) {
                    // Save to /settings.json
                    StaticJsonDocument<512> doc;
                    doc["ssid"] = ssid;
                    doc["password"] = password;
                    doc["name"] = displayName;
                    doc["transport"] = transportEnabled;
                    File f = SPIFFS.open("/settings.json", "w");
                    if (f) {
                        serializeJson(doc, f);
                        f.close();
                    }
                    dirty = true;
                } else {
                    settingsEditing = true;
                    switch (settingsSel) {
                        case 0: settingsEditTarget = ssid; settingsEditMax = sizeof(ssid) - 1; break;
                        case 1: settingsEditTarget = password; settingsEditMax = sizeof(password) - 1; break;
                        case 2: settingsEditTarget = displayName; settingsEditMax = sizeof(displayName) - 1; break;
                    }
                }
                dirty = true;
            }
        } else {
            if (lastChar == KEY_ENTER) {
                settingsEditing = false;
                settingsEditTarget = nullptr;
                dirty = true;
            } else if (lastChar == KEY_BACKSPACE || lastChar == KEY_DEL) {
                if (settingsEditTarget && strlen(settingsEditTarget) > 0) {
                    settingsEditTarget[strlen(settingsEditTarget) - 1] = '\0';
                    dirty = true;
                }
            } else if (lastChar >= 32 && lastChar < 127 && settingsEditTarget) {
                if (strlen(settingsEditTarget) < settingsEditMax) {
                    size_t len = strlen(settingsEditTarget);
                    settingsEditTarget[len] = lastChar;
                    settingsEditTarget[len + 1] = '\0';
                    dirty = true;
                }
            }
        }
    }
}

// ------------------------------------------------------------------
// Status
// ------------------------------------------------------------------
void ReticuleM::runStatus() {
    bool dirty = stateChanged;
    if (dirty) {
        clearScreen();
        drawHeader("Status");
        M5Cardputer.Display.setTextFont(&fonts::FreeMono9pt7b);
        M5Cardputer.Display.setTextDatum(top_left);
        M5Cardputer.Display.setTextColor(TFT_WHITE);
        int y = 16;
        char lineBuf[64];
        
        if (wifiConnected) {
            snprintf(lineBuf, sizeof(lineBuf), "WiFi:  %s", WiFi.localIP().toString().c_str());
        } else {
            snprintf(lineBuf, sizeof(lineBuf), "WiFi:  Disconnected");
        }
        M5Cardputer.Display.drawString(lineBuf, 2, y);
        y += 14;
        
        // LoRa status line
        if (loraError[0] != '\0') {
            snprintf(lineBuf, sizeof(lineBuf), "LoRa:  %s", loraError);
        } else if (loraOnline && !std::isnan(loraRSSI)) {
            snprintf(lineBuf, sizeof(lineBuf), "LoRa:  RSSI %.0f dBm  SNR %.1f", loraRSSI, loraSNR);
        } else if (loraOnline) {
            snprintf(lineBuf, sizeof(lineBuf), "LoRa:  Online  (no signal yet)");
        } else {
            snprintf(lineBuf, sizeof(lineBuf), "LoRa:  Offline");
        }
        M5Cardputer.Display.drawString(lineBuf, 2, y);
        y += 14;
        
        snprintf(lineBuf, sizeof(lineBuf), "Hash:  %.28s", ownHash);
        M5Cardputer.Display.drawString(lineBuf, 2, y);
        y += 14;
        snprintf(lineBuf, sizeof(lineBuf), "Msgs:  %d/%d", messageCount, MAX_MESSAGES);
        M5Cardputer.Display.drawString(lineBuf, 2, y);
        y += 14;
        snprintf(lineBuf, sizeof(lineBuf), "Contacts: %d", contactCount);
        M5Cardputer.Display.drawString(lineBuf, 2, y);
        y += 14;
        snprintf(lineBuf, sizeof(lineBuf), "Status: %s", nodeStatus);
        M5Cardputer.Display.drawString(lineBuf, 2, y);
        y += 14;
        snprintf(lineBuf, sizeof(lineBuf), "Name:  %s", displayName);
        M5Cardputer.Display.drawString(lineBuf, 2, y);
        
        drawFooter("`=Esc");
        dirty = false;
    }
    if (keyPressed && (lastChar == KEY_ESC)) {
        state = AppState::HOME;
        dirty = true;
    }
}

// ------------------------------------------------------------------
// Keyboard Processing
// ------------------------------------------------------------------
void ReticuleM::processKeyboard() {
    keyPressed = false;
    lastChar = 0;

    if (!M5Cardputer.Keyboard.isChange()) return;
    if (!M5Cardputer.Keyboard.isPressed()) return;

    Keyboard_Class::KeysState status = M5Cardputer.Keyboard.keysState();
    fnDown = status.fn;
    ctrlDown = status.ctrl;

    // Special flags already decoded by library
    if (status.del) { keyPressed = true; lastChar = KEY_BACKSPACE; }
    if (status.enter) { keyPressed = true; lastChar = KEY_ENTER; }
    if (status.tab) { keyPressed = true; lastChar = KEY_TAB; }
    if (status.space) { keyPressed = true; lastChar = ' '; }

    // Map raw physical positions to semantic keys (Cardputer ADV layout)
    for (auto keyPos : M5Cardputer.Keyboard.keyList()) {
        // Skip modifier positions
        if ((keyPos.x == 0 && keyPos.y == 2) ||  // FN
            (keyPos.x == 1 && keyPos.y == 2) ||  // SHIFT
            (keyPos.x == 0 && keyPos.y == 3) ||  // CTRL
            (keyPos.x == 1 && keyPos.y == 3) ||  // OPT
            (keyPos.x == 2 && keyPos.y == 3)) {   // ALT
            continue;
        }

        // Dedicated arrow keys (physical positions on ADV keyboard)
        if (keyPos.x == 11 && keyPos.y == 2) { lastChar = KEY_UP; keyPressed = true; }
        else if (keyPos.x == 11 && keyPos.y == 3) { lastChar = KEY_DOWN; keyPressed = true; }
        else if (keyPos.x == 10 && keyPos.y == 3) { lastChar = KEY_LEFT; keyPressed = true; }
        else if (keyPos.x == 12 && keyPos.y == 3) { lastChar = KEY_RIGHT; keyPressed = true; }
        // ESC key (top-left physical key, labeled ` or ~)
        else if (keyPos.x == 0 && keyPos.y == 0) { lastChar = KEY_ESC; keyPressed = true; }
        // Enter and Backspace are already covered by status flags, but map anyway
        else if (keyPos.x == 13 && keyPos.y == 2) { lastChar = KEY_ENTER; keyPressed = true; }
        else if (keyPos.x == 13 && keyPos.y == 0) { lastChar = KEY_BACKSPACE; keyPressed = true; }
        else {
            // Regular printable character
            auto kv = M5Cardputer.Keyboard.getKeyValue(keyPos);
            if (status.shift || status.ctrl || M5Cardputer.Keyboard.capslocked()) {
                lastChar = kv.value_second;
            } else {
                lastChar = kv.value_first;
            }
            keyPressed = true;
        }
        break; // only act on first non-modifier key per frame
    }

    if (keyPressed && lastChar >= 32 && lastChar < 127) {
        cursorPos++;
    } else if ((lastChar == KEY_BACKSPACE || lastChar == KEY_DEL) && cursorPos > 0) {
        cursorPos--;
    }
}

// ------------------------------------------------------------------
// wrapText — pixel-exact line breaking
// ------------------------------------------------------------------
void ReticuleM::wrapText(const String& text, int maxPixelWidth, std::vector<String>& lines) {
    lines.clear();
    int start = 0;
    M5Cardputer.Display.setTextFont(&fonts::FreeMono9pt7b);
    while (start < text.length()) {
        int end = text.length();
        // Try to find a break point within the width
        while (end > start && M5Cardputer.Display.textWidth(text.substring(start, end)) > maxPixelWidth) {
            end--;
        }
        // If even a single character overflows, force break at character boundary
        if (end == start) {
            end = start + 1;
            while (end < text.length() && M5Cardputer.Display.textWidth(text.substring(start, end + 1)) <= maxPixelWidth) {
                end++;
            }
        }
        lines.push_back(text.substring(start, end));
        start = end;
    }
}

// ------------------------------------------------------------------
// UI Helpers
// ------------------------------------------------------------------
void ReticuleM::clearScreen(uint16_t color) {
    M5Cardputer.Display.fillScreen(color);
}

void ReticuleM::drawHeader(const char* title) {
    M5Cardputer.Display.fillRect(0, 0, SCREEN_W, 14, TFT_NAVY);
    M5Cardputer.Display.setTextFont(&fonts::FreeMono9pt7b);
    M5Cardputer.Display.setTextDatum(middle_left);
    M5Cardputer.Display.setTextColor(TFT_WHITE);
    M5Cardputer.Display.drawString(title, 4, 7);
}

void ReticuleM::drawFooter(const char* hint) {
    M5Cardputer.Display.fillRect(0, SCREEN_H - 12, SCREEN_W, 12, TFT_DARKGREY);
    M5Cardputer.Display.setTextFont(&fonts::FreeMono9pt7b);
    M5Cardputer.Display.setTextDatum(middle_left);
    M5Cardputer.Display.setTextColor(TFT_WHITE);
    M5Cardputer.Display.drawString(hint, 2, SCREEN_H - 6);
}

void ReticuleM::drawStatusBar() {
    // LoRa indicator (left)
    bool loraOk = loraOnline && inboxDest;
    uint16_t loraColor = loraOk ? TFT_GREEN : TFT_RED;
    M5Cardputer.Display.fillRect(SCREEN_W - 22, 2, 10, 10, loraColor);
    
    // WiFi indicator (right)
    bool wifiOk = wifiConnected && inboxDest;
    uint16_t wifiColor = wifiOk ? TFT_GREEN : TFT_RED;
    M5Cardputer.Display.fillRect(SCREEN_W - 10, 2, 8, 10, wifiColor);
}

void ReticuleM::drawMenu(const char** items, int count, int selected) {
    const int lineHeight = 13;
    int startY = 18;
    M5Cardputer.Display.setTextFont(&fonts::FreeMono9pt7b);
    for (int i = 0; i < count; i++) {
        int y = startY + i * lineHeight;
        if (i == selected) {
            M5Cardputer.Display.fillRect(0, y, SCREEN_W, lineHeight, TFT_DARKCYAN);
            M5Cardputer.Display.setTextColor(TFT_BLACK);
        } else {
            M5Cardputer.Display.setTextColor(TFT_WHITE);
        }
        M5Cardputer.Display.setTextDatum(top_left);
        M5Cardputer.Display.drawString(String("> ") + items[i], 4, y);
    }
}

void ReticuleM::drawMessageList() {
    if (messageCount == 0) {
        M5Cardputer.Display.setTextFont(&fonts::FreeMono9pt7b);
        M5Cardputer.Display.setTextDatum(middle_center);
        M5Cardputer.Display.setTextColor(TFT_DARKGREY);
        M5Cardputer.Display.drawString("No messages", SCREEN_W / 2, SCREEN_H / 2);
        return;
    }
    const int lineHeight = 13;
    int start = max(0, messageSelected - 7);
    for (int i = start; i < messageCount && (i - start) < 9; i++) {
        int y = 16 + (i - start) * lineHeight;
        Message m = messages[i];
        bool active = (i == messageSelected);
        if (active) {
            M5Cardputer.Display.fillRect(0, y, SCREEN_W, lineHeight, TFT_DARKCYAN);
            M5Cardputer.Display.setTextColor(TFT_BLACK);
        } else {
            M5Cardputer.Display.setTextColor(m.read ? TFT_LIGHTGREY : TFT_WHITE);
        }
        M5Cardputer.Display.setTextFont(&fonts::FreeMono9pt7b);
        M5Cardputer.Display.setTextDatum(top_left);
        char lineBuf[48];
        const char* prefix = m.outgoing ? ">" : (m.read ? " " : "*");
        snprintf(lineBuf, sizeof(lineBuf), "%s %.10s: %.24s", prefix, m.sender, m.content);
        M5Cardputer.Display.drawString(lineBuf, 2, y);
    }
}
