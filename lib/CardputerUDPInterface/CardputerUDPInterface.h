#pragma once

#include <microReticulum/Interface.h>
#include <microReticulum/Bytes.h>
#include <microReticulum/Type.h>

#ifdef ARDUINO
#include <WiFi.h>
#include <WiFiUdp.h>
#endif

#include <stdint.h>

#ifndef DEFAULT_UDP_PORT
#define DEFAULT_UDP_PORT  4242
#endif
#ifndef DEFAULT_UDP_LOCAL_PORT
#define DEFAULT_UDP_LOCAL_PORT  DEFAULT_UDP_PORT
#endif
#ifndef DEFAULT_UDP_REMOTE_PORT
#define DEFAULT_UDP_REMOTE_PORT DEFAULT_UDP_PORT
#endif
#ifndef DEFAULT_UDP_LOCAL_HOST
#define DEFAULT_UDP_LOCAL_HOST "0.0.0.0"
#endif
#ifndef DEFAULT_UDP_REMOTE_HOST
#define DEFAULT_UDP_REMOTE_HOST "255.255.255.255"
#endif

class CardputerUDPInterface : public RNS::InterfaceImpl {
public:
    static const uint32_t BITRATE_GUESS = 10*1000*1000;

    CardputerUDPInterface(const char* name = "CardputerUDPInterface");
    virtual ~CardputerUDPInterface();

    void setWiFiCredentials(const char* ssid, const char* password);
    void setRemoteHost(const char* host, int port);

    virtual bool start();
    virtual void stop();
    virtual void loop();

    virtual inline std::string toString() const {
        return "CardputerUDPInterface[" + _name + "/" + _local_host + ":" + std::to_string(_local_port) + "]";
    }

protected:
    virtual bool send_outgoing(const RNS::Bytes& data);
    void on_incoming(const RNS::Bytes& data);

private:
    RNS::Bytes _buffer;
    std::string _wifi_ssid;
    std::string _wifi_password;
    std::string _local_host = DEFAULT_UDP_LOCAL_HOST;
    int _local_port = DEFAULT_UDP_LOCAL_PORT;
    std::string _remote_host = DEFAULT_UDP_REMOTE_HOST;
    int _remote_port = DEFAULT_UDP_REMOTE_PORT;

#ifdef ARDUINO
    WiFiUDP udp;
#else
    int _socket = -1;
    in_addr_t _local_address = INADDR_ANY;
    in_addr_t _remote_address = INADDR_NONE;
#endif
};
