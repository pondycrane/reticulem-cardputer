#ifndef RETICULEM_APP_H
#define RETICULEM_APP_H

#include <M5Cardputer.h>
#include <microReticulum.h>
#include <microStore/Adapters/NoopFileSystem.h>
#include <MsgPack.h>
#include <CardputerUDPInterface.h>
#include <memory>
#include <LoRaInterface.h>

// Additional key constants (Cardputer lacks dedicated arrow keys)
#define KEY_UP     0x52
#define KEY_DOWN   0x51
#define KEY_LEFT   0x50
#define KEY_RIGHT   0x4F
#define KEY_DEL    0x4C
#define KEY_ESC    0x1B

// Display geometry
constexpr int SCREEN_W = 240;
constexpr int SCREEN_H = 135;

// App states
enum class AppState {
    SPLASH,
    HOME,
    INBOX,
    COMPOSE,
    CONTACTS,
    SETTINGS,
    STATUS
};

// Message record
struct Message {
    uint32_t id;
    uint32_t timestamp;
    char sender[64];
    char recipient[64];
    char content[256];
    bool outgoing;
    bool read;
};

// Contact record
struct Contact {
    char name[32];
    char hash[128];  // Reticulum truncated hex hash (display)
    RNS::Bytes fullHash; // Full 32-byte identity hash for Identity::recall()
};

// Forward declare custom announce handler
class ReticuleMAnnounceHandler;

class ReticuleM {
public:
    ReticuleM();
    void begin();
    void update();

    // ---- Callbacks (must be accessible from C glue) ----
    void onPacketReceived(const RNS::Bytes& data, const RNS::Packet& packet);
    void onAnnounceReceived(const RNS::Bytes& destination_hash,
                            const RNS::Identity& announced_identity,
                            const RNS::Bytes& app_data);

private:
    // ---- Hardware ----
    void initDisplay();
    void initFS();
    void initWiFi();
    
    // ---- Reticulum ----
    void initReticulum();
    void reticulumLoop();
    void loadIdentity();
    void saveIdentity();
    void performAnnounce();
    void flushPendingSend();
    void sendNativeMessage(const char* recipient_hex, const char* body);
    void doSendNativeMessage(const RNS::Bytes& hash, const char* body);
    
    // ---- Async WiFi ----
    void checkWiFiConnection();
    void checkLoRaConnection();
    
    // ---- State Machine ----
    AppState state;
    AppState lastState;
    bool stateChanged;  // true on transition
    void runSplash();
    void runHome();
    void runInbox();
    void runCompose();
    void runContacts();
    void runSettings();
    void runStatus();
    
    // ---- UI ----
    void clearScreen(uint16_t color = TFT_BLACK);
    void drawHeader(const char* title);
    void drawFooter(const char* hint);
    void drawStatusBar();
    void drawMenu(const char** items, int count, int selected);
    void drawMessageList();
    void drawComposeScreen();
    void renderSplash();
    
    // ---- Keyboard ----
    void processKeyboard();
    bool keyPressed;
    char lastChar;
    bool fnDown;
    bool ctrlDown;
    int cursorPos;
    
    // ---- Data ----
    static constexpr int MAX_MESSAGES = 32;
    static constexpr int MAX_CONTACTS = 16;
    Message messages[MAX_MESSAGES];
    int messageCount;
    int messageSelected;
    Contact contacts[MAX_CONTACTS];
    int contactCount;
    int contactSelected;
    
    char composeTo[128];
    char composeBody[256];
    int composeField; // 0=to, 1=body
    
    // ---- Settings ----
    char ssid[64];
    char password[64];
    char displayName[32];
    bool transportEnabled;
    
    // ---- UI State (replaces static locals) ----
    int homeSelected = 0;
    int settingsSel = 0;
    bool settingsEditing = false;
    char* settingsEditTarget = nullptr;
    size_t settingsEditMax = 0;
    
    // ---- Message ID counter ----
    uint32_t nextMsgId = 1;
    
    // ---- Timing ----
    unsigned long lastBlink;
    bool cursorVisible;
    unsigned long splashStart;
    unsigned long lastAnnounce;
    unsigned long lastStatusUpdate;
    
    // ---- Pending network operations ----
    char pendingSendTo[128];
    char pendingSendBody[256];
    bool pendingSendActive;
    unsigned long pendingSendStart;
    
    // ---- Async WiFi ----
    unsigned long wifiConnectStart = 0;
    
    // Reticulum objects
    RNS::Reticulum reticulum;
    RNS::Identity identity;
    RNS::Destination inboxDest{RNS::Type::NONE};
    RNS::HAnnounceHandler announceHandler;
    
    // UDP interface wrapper + impl pointer (for credential setup)
    RNS::Interface udpInterface{RNS::Type::NONE};
    std::unique_ptr<CardputerUDPInterface> udpImpl;

    // LoRa interface — Cap LoRa-1262 (U214) on EXT 2.54-14P header
    RNS::Interface loraInterface{RNS::Type::NONE};
    std::unique_ptr<LoRaInterface> loraImpl;
    
    // ---- Status info ----
    bool wifiConnected;
    bool loraOnline;
    float loraRSSI;
    float loraSNR;
    char ownHash[128];
    char nodeStatus[64];
    int peerCount;
};

#endif // RETICULEM_APP_H
