#include "MessageStore.h"

MessageStore::MessageStore() {
    loadMessages();
    loadContacts();
}

void MessageStore::addMessage(const Message& msg) {
    if (_messages.size() < MAX_MESSAGES) {
        _messages.push_back(msg);
    }
}

bool MessageStore::removeMessage(int index) {
    if (index >= 0 && index < (int)_messages.size()) {
        _messages.erase(_messages.begin() + index);
        return true;
    }
    return false;
}

void MessageStore::clearMessages() {
    _messages.clear();
}

void MessageStore::addOrUpdateContact(const char* hash, const char* name, const RNS::Bytes& fullHash) {
    // Check if contact already exists
    for (auto& contact : _contacts) {
        if (strcmp(contact.hash, hash) == 0) {
            strlcpy(contact.name, name, sizeof(contact.name));
            if (fullHash.size() > 0) {
                contact.fullHash = fullHash;
            }
            return;
        }
    }
    
    // Add new contact if not at capacity
    if (_contacts.size() < MAX_CONTACTS) {
        Contact c{};
        strlcpy(c.name, name, sizeof(c.name));
        strlcpy(c.hash, hash, sizeof(c.hash));
        if (fullHash.size() > 0) {
            c.fullHash = fullHash;
        }
        _contacts.push_back(c);
    }
}

const Contact* MessageStore::findContactByHash(const char* hash) const {
    for (const auto& contact : _contacts) {
        if (strcmp(contact.hash, hash) == 0) {
            return &contact;
        }
    }
    return nullptr;
}

bool MessageStore::removeContact(int index) {
    if (index >= 0 && index < (int)_contacts.size()) {
        _contacts.erase(_contacts.begin() + index);
        return true;
    }
    return false;
}

void MessageStore::clearContacts() {
    _contacts.clear();
}

bool MessageStore::loadMessages() {
    File f = SPIFFS.open("/messages.json", "r");
    if (!f) return false;
    
    StaticJsonDocument<4096> doc;
    DeserializationError err = deserializeJson(doc, f);
    f.close();
    
    if (err) return false;
    
    _messages.clear();
    JsonArray msgArray = doc["messages"].as<JsonArray>();
    for (JsonObject msgObj : msgArray) {
        Message msg{};
        msg.id = msgObj["id"];
        msg.timestamp = msgObj["timestamp"];
        strlcpy(msg.sender, msgObj["sender"], sizeof(msg.sender));
        strlcpy(msg.recipient, msgObj["recipient"], sizeof(msg.recipient));
        strlcpy(msg.content, msgObj["content"], sizeof(msg.content));
        msg.outgoing = msgObj["outgoing"];
        msg.read = msgObj["read"];
        _messages.push_back(msg);
        
        // Track highest ID
        if (msg.id >= _nextMsgId) {
            _nextMsgId = msg.id + 1;
        }
    }
    
    return true;
}

bool MessageStore::saveMessages() {
    StaticJsonDocument<4096> doc;
    JsonArray msgArray = doc.createNestedArray("messages");
    
    for (const auto& msg : _messages) {
        JsonObject msgObj = msgArray.createNestedObject();
        msgObj["id"] = msg.id;
        msgObj["timestamp"] = msg.timestamp;
        msgObj["sender"] = msg.sender;
        msgObj["recipient"] = msg.recipient;
        msgObj["content"] = msg.content;
        msgObj["outgoing"] = msg.outgoing;
        msgObj["read"] = msg.read;
    }
    
    File f = SPIFFS.open("/messages.json", "w");
    if (!f) return false;
    
    serializeJson(doc, f);
    f.close();
    return true;
}

bool MessageStore::loadContacts() {
    File f = SPIFFS.open("/contacts.json", "r");
    if (!f) return false;
    
    StaticJsonDocument<2048> doc;
    DeserializationError err = deserializeJson(doc, f);
    f.close();
    
    if (err) return false;
    
    _contacts.clear();
    JsonArray contactArray = doc["contacts"].as<JsonArray>();
    for (JsonObject contactObj : contactArray) {
        Contact contact{};
        strlcpy(contact.name, contactObj["name"], sizeof(contact.name));
        strlcpy(contact.hash, contactObj["hash"], sizeof(contact.hash));
        // Note: fullHash cannot be serialized easily, will be loaded from announces
        _contacts.push_back(contact);
    }
    
    return true;
}

bool MessageStore::saveContacts() {
    StaticJsonDocument<2048> doc;
    JsonArray contactArray = doc.createNestedArray("contacts");
    
    for (const auto& contact : _contacts) {
        JsonObject contactObj = contactArray.createNestedObject();
        contactObj["name"] = contact.name;
        contactObj["hash"] = contact.hash;
        // Note: fullHash omitted from serialization
    }
    
    File f = SPIFFS.open("/contacts.json", "w");
    if (!f) return false;
    
    serializeJson(doc, f);
    f.close();
    return true;
}