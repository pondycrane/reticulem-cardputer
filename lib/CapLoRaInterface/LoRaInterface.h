#pragma once

#include <microReticulum/Interface.h>
#include <microReticulum/Bytes.h>
#include <microReticulum/Type.h>

#ifdef ARDUINO
#include <SPI.h>
#include <RadioLib.h>
#include <memory>
#endif

#include <stdint.h>
#include <cmath>

class LoRaInterface : public RNS::InterfaceImpl {

public:

	LoRaInterface(const char* name = "LoRaInterface");
	virtual ~LoRaInterface();

	virtual bool start();
	virtual void stop();
	virtual void loop();

public:
	// Signal quality accessors — updated after each received packet
	float getRSSI() const { return _lastRSSI; }
	float getSNR() const { return _lastSNR; }
	bool isOnline() const { return _online; }

private:
	virtual bool send_outgoing(const RNS::Bytes& data);
	void on_incoming(const RNS::Bytes& data);

public:
	// Split-packet protocol constants
	static constexpr uint8_t HEADER_SPLIT     = 0x08;  // bit 3: split-packet flag
	static constexpr uint8_t HEADER_SEQ_MASK  = 0x07;  // bits 2:0: sequence number
	static constexpr uint8_t SEQ_UNSET        = 0xFF;  // sentinel: no split in progress
	static constexpr int     LORA_MAX_PAYLOAD = 254;   // 255 - 1 header byte

private:
	//uint8_t buffer[Type::Reticulum::MTU] = {0};
	const uint8_t message_count = 0;
	RNS::Bytes buffer;
	static constexpr size_t MAX_BUFFER_SIZE = 1024;  // Limit reassembly buffer

	// Last received signal metrics (RadioLib units: dBm, dB)
	float _lastRSSI = NAN;
	float _lastSNR  = NAN;

	uint8_t _rx_seq     = SEQ_UNSET;  // sequence of split RX in progress
	uint8_t _tx_seq_ctr = 0;          // rolling TX split sequence counter

	// Radio parameters (RadioLib units: MHz, kHz)
	// Override via -DLORA_FREQ=868.0 from build flags
	#ifndef LORA_FREQ
	#define LORA_FREQ 915.0
	#endif
	const float frequency = LORA_FREQ;   // MHz
	const float bandwidth = 125.0;   // kHz
	const int   spreading = 8;
	const int   coding    = 5;
	const int   power     = 17;      // dBm

#ifdef ARDUINO
	std::unique_ptr<Module> _module;
	std::unique_ptr<PhysicalLayer> _radio;
	int            _pa_mode_pin = -1;    // V4 FEM PA mode pin; -1 = not present
#endif

};
