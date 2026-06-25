#ifndef RETICULEM_MESSAGESTORE_H
#define RETICULEM_MESSAGESTORE_H

#include <SPIFFS.h>
#include <ArduinoJson.h>
#include <vector>
#include <microReticulum.h>

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

class MessageStore {
public:
    static constexpr int MAX_MESSAGES = 32;
    static constexpr int MAX_CONTACTS = 16;
    
    MessageStore();
    
    // Message operations
    int messageCount() const { return _messages.size(); }
    const Message& getMessage(int index) const { return _messages[index]; }
    void addMessage(const Message& msg);
    bool removeMessage(int index);
    void clearMessages();
    
    // Contact operations
    int contactCount() const { return _contacts.size(); }
    const Contact& getContact(int index) const { return _contacts[index]; }
    void addOrUpdateContact(const char* hash, const char* name, const RNS::Bytes& fullHash = RNS::Bytes());
    const Contact* findContactByHash(const char* hash) const;
    bool removeContact(int index);
    void clearContacts();
    
    // Persistence
    bool loadMessages();
    bool saveMessages();
    bool loadContacts();
    bool saveContacts();
    
    void markMessageRead(int index) { if (index >= 0 && index < (int)_messages.size()) _messages[index].read = true; }
    uint32_t getNextMessageId() { return _nextMsgId++; }
    
private:
    std::vector<Message> _messages;
    std::vector<Contact> _contacts;
    
    uint32_t _nextMsgId = 1;
};

#endif // RETICULEM_MESSAGESTORE_H