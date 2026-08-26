#include "Socket.h"
#include <cstring>

#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
#else
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <unistd.h>
#include <fcntl.h>
#endif

UdpSocket::UdpSocket() : initialized(false) {
#ifdef _WIN32
    WSADATA wsaData;
    if (WSAStartup(MAKEWORD(2, 2), &wsaData) != 0) {
        sockHandle = 0;
        return;
    }
    SOCKET s = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (s == INVALID_SOCKET) {
        WSACleanup();
        return;
    }
    u_long nonBlocking = 1;
    ioctlsocket(s, FIONBIO, &nonBlocking);
    sockHandle = (uintptr_t)s;
    initialized = true;
#else
    int s = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (s < 0) {
        return;
    }
    int flags = fcntl(s, F_GETFL, 0);
    fcntl(s, F_SETFL, flags | O_NONBLOCK);
    sockHandle = s;
    initialized = true;
#endif
}

UdpSocket::~UdpSocket() {
    if (!initialized) {
        return;
    }
#ifdef _WIN32
    closesocket((SOCKET)sockHandle);
    WSACleanup();
#else
    close(sockHandle);
#endif
}

bool UdpSocket::Bind(uint16_t port) {
    if (!initialized) {
        return false;
    }
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = INADDR_ANY;
    addr.sin_port = htons(port);
#ifdef _WIN32
    return bind((SOCKET)sockHandle, (sockaddr*)&addr, sizeof(addr)) == 0;
#else
    return bind(sockHandle, (sockaddr*)&addr, sizeof(addr)) == 0;
#endif
}

bool UdpSocket::SendTo(const std::string& ip, uint16_t port, const uint8_t* data, size_t len) {
    if (!initialized) {
        return false;
    }
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(port);
    inet_pton(AF_INET, ip.c_str(), &addr.sin_addr);
#ifdef _WIN32
    int sent = sendto((SOCKET)sockHandle, (const char*)data, (int)len, 0, (sockaddr*)&addr, sizeof(addr));
#else
    ssize_t sent = sendto(sockHandle, data, len, 0, (sockaddr*)&addr, sizeof(addr));
#endif
    return sent == (int)len || sent == (ssize_t)len;
}

int UdpSocket::ReceiveFrom(uint8_t* outBuffer, size_t bufferCapacity, std::string& outFromIp, uint16_t& outFromPort) {
    if (!initialized) {
        return -1;
    }
    sockaddr_in fromAddr{};
#ifdef _WIN32
    int fromLen = sizeof(fromAddr);
    int received = recvfrom((SOCKET)sockHandle, (char*)outBuffer, (int)bufferCapacity, 0, (sockaddr*)&fromAddr, &fromLen);
    if (received <= 0) {
        return -1;
    }
#else
    socklen_t fromLen = sizeof(fromAddr);
    ssize_t received = recvfrom(sockHandle, outBuffer, bufferCapacity, 0, (sockaddr*)&fromAddr, &fromLen);
    if (received <= 0) {
        return -1;
    }
#endif
    char ipStr[INET_ADDRSTRLEN];
    inet_ntop(AF_INET, &fromAddr.sin_addr, ipStr, sizeof(ipStr));
    outFromIp = ipStr;
    outFromPort = ntohs(fromAddr.sin_port);
    return (int)received;
}
