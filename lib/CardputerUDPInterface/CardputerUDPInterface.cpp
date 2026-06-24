#include "CardputerUDPInterface.h"

#include <microReticulum/Transport.h>
#include <microReticulum/Log.h>

#include <memory.h>

#ifndef ARDUINO
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <netdb.h>
#include <arpa/inet.h>
#include <unistd.h>
#include <errno.h>
#endif

using namespace RNS;

CardputerUDPInterface::CardputerUDPInterface(const char* name /*= "CardputerUDPInterface"*/) : RNS::InterfaceImpl(name) {
    _IN = true;
    _OUT = true;
    _bitrate = BITRATE_GUESS;
    _HW_MTU = 1064;
}

CardputerUDPInterface::~CardputerUDPInterface() {
    stop();
}

void CardputerUDPInterface::setWiFiCredentials(const char* ssid, const char* password) {
    if (ssid) _wifi_ssid = ssid;
    if (password) _wifi_password = password;
}

void CardputerUDPInterface::setRemoteHost(const char* host, int port) {
    if (host) _remote_host = host;
    _remote_port = port;
}

bool CardputerUDPInterface::start() {
    _online = false;

    TRACEF("CardputerUDPInterface: local host: %s", _local_host.c_str());
    TRACEF("CardputerUDPInterface: local port: %d", _local_port);
    TRACEF("CardputerUDPInterface: remote host: %s", _remote_host.c_str());
    TRACEF("CardputerUDPInterface: remote port: %d", _remote_port);

#ifdef ARDUINO
    TRACEF("CardputerUDPInterface: wifi ssid: %s", _wifi_ssid.c_str());

    // Only connect WiFi if not already connected
    if (WiFi.status() != WL_CONNECTED) {
        if (_wifi_ssid.empty()) {
            ERROR("CardputerUDPInterface: no WiFi SSID configured");
            return false;
        }
        WiFi.begin(_wifi_ssid.c_str(), _wifi_password.c_str());
        Serial.print("Connecting to ");
        Serial.print(_wifi_ssid.c_str());
        int retries = 40; // ~20 seconds
        while (WiFi.status() != WL_CONNECTED && retries-- > 0) {
            delay(500);
            Serial.print(".");
        }
        Serial.println("");
        if (WiFi.status() != WL_CONNECTED) {
            ERROR("CardputerUDPInterface: WiFi connection failed");
            return false;
        }
        Serial.print("Connected, IP: ");
        Serial.println(WiFi.localIP());
    } else {
        INFO("CardputerUDPInterface: WiFi already connected");
    }

    udp.begin(_local_port);
#else
    struct in_addr local_addr;
    if (inet_aton(_local_host.c_str(), &local_addr) == 0) {
        struct hostent* host_ent = gethostbyname(_local_host.c_str());
        if (host_ent == nullptr || host_ent->h_addr_list[0] == nullptr) {
            ERRORF("Unable to resolve local host %s", _local_host.c_str());
            return false;
        }
        _local_address = *((in_addr_t*)(host_ent->h_addr_list[0]));
    } else {
        _local_address = local_addr.s_addr;
    }

    struct in_addr remote_addr;
    if (inet_aton(_remote_host.c_str(), &remote_addr) == 0) {
        struct hostent* host_ent = gethostbyname(_remote_host.c_str());
        if (host_ent == nullptr || host_ent->h_addr_list[0] == nullptr) {
            ERRORF("Unable to resolve remote host %s", _remote_host.c_str());
            return false;
        }
        _remote_address = *((in_addr_t*)(host_ent->h_addr_list[0]));
    } else {
        _remote_address = remote_addr.s_addr;
    }

    _socket = socket(PF_INET, SOCK_DGRAM, 0);
    if (_socket < 0) {
        ERRORF("Unable to create socket with error %d", errno);
        return false;
    }

    int broadcast = 1;
    setsockopt(_socket, SOL_SOCKET, SO_BROADCAST, &broadcast, sizeof(broadcast));

    int reuse = 1;
    setsockopt(_socket, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse));
#ifdef SO_REUSEPORT
    setsockopt(_socket, SOL_SOCKET, SO_REUSEPORT, &reuse, sizeof(reuse));
#endif

    sockaddr_in bind_addr;
    bind_addr.sin_family = AF_INET;
    bind_addr.sin_addr.s_addr = _local_address;
    bind_addr.sin_port = htons(_local_port);
    if (bind(_socket, (struct sockaddr*)&bind_addr, sizeof(bind_addr)) == -1) {
        close(_socket);
        _socket = -1;
        ERRORF("Unable to bind socket with error %d", errno);
        return false;
    }
#endif

    _online = true;
    INFO("CardputerUDPInterface online");
    return true;
}

void CardputerUDPInterface::stop() {
#ifdef ARDUINO
#else
    if (_socket > -1) {
        close(_socket);
        _socket = -1;
    }
#endif
    _online = false;
}

void CardputerUDPInterface::loop() {
    if (_online) {
#ifdef ARDUINO
        int len = udp.parsePacket();
        if (len > 0) {
            if (len > (int)Type::Reticulum::MTU) len = Type::Reticulum::MTU;
            _buffer.resize(len);
            udp.read(_buffer.writable(len), len);
            on_incoming(_buffer);
        }
#else
        while (true) {
            sockaddr_in src_addr{};
            socklen_t src_addr_len = sizeof(src_addr);
            ssize_t len = recvfrom(_socket,
                                   _buffer.writable(_HW_MTU),
                                   _HW_MTU,
                                   MSG_DONTWAIT,
                                   (struct sockaddr*)&src_addr,
                                   &src_addr_len);
            if (len <= 0) break;
            _buffer.resize(static_cast<size_t>(len));
            on_incoming(_buffer);
        }
#endif
    }
}

bool CardputerUDPInterface::send_outgoing(const Bytes& data) {
    DEBUGF("%s.send_outgoing: %lu bytes", toString().c_str(), (unsigned long)data.size());
    bool success = true;
    try {
        if (_online) {
#ifdef ARDUINO
            if (!udp.beginPacket(_remote_host.c_str(), _remote_port)) {
                WARNING("CardputerUDPInterface: beginPacket failed");
                return false;
            }
            size_t written = udp.write(data.data(), data.size());
            if (written != data.size()) {
                WARNINGF("CardputerUDPInterface: only wrote %u/%lu bytes", written, (unsigned long)data.size());
                udp.endPacket();
                return false;
            }
            if (udp.endPacket() == 0) {
                WARNING("CardputerUDPInterface: endPacket failed (0 bytes sent)");
                return false;
            }
#else
            sockaddr_in sock_addr;
            sock_addr.sin_family = AF_INET;
            sock_addr.sin_addr.s_addr = _remote_address;
            sock_addr.sin_port = htons(_remote_port);
            int sent = sendto(_socket, data.data(), data.size(), 0, (struct sockaddr*)&sock_addr, sizeof(sock_addr));
            if (sent != (int)data.size()) {
                WARNINGF("Failed sending %d bytes to %s:%d", sent, _remote_host.c_str(), _remote_port);
                success = false;
            }
#endif
        }
        InterfaceImpl::handle_outgoing(data);
    }
    catch (const std::exception& e) {
        ERRORF("Transmit exception on %s: %s", toString().c_str(), e.what());
        success = false;
    }
    return success;
}

void CardputerUDPInterface::on_incoming(const Bytes& data) {
    DEBUGF("%s.on_incoming: %lu bytes", toString().c_str(), (unsigned long)data.size());
    InterfaceImpl::handle_incoming(data);
}
