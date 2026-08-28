#pragma once

#include <cstdint>
#include <cstddef>
#include <string>

class UdpSocket {
public:
    UdpSocket();
    ~UdpSocket();

    bool Bind(uint16_t port);
    bool SendTo(const std::string& ip, uint16_t port, const uint8_t* data, size_t len);
    int ReceiveFrom(uint8_t* outBuffer, size_t bufferCapacity, std::string& outFromIp, uint16_t& outFromPort);

private:
#ifdef _WIN32
    uintptr_t sockHandle; // SOCKET, stored as uintptr_t to avoid pulling <winsock2.h> into this header
#else
    int sockHandle;
#endif
    bool initialized;
};
