#pragma once

#include <cstdint>
#include <cstring>
#include <vector>

template<typename T>
void SerializeStruct(const T& value, std::vector<uint8_t>& outBuffer) {
    size_t offset = outBuffer.size();
    outBuffer.resize(offset + sizeof(T));
    std::memcpy(outBuffer.data() + offset, &value, sizeof(T));
}

template<typename T>
bool DeserializeStruct(const uint8_t* data, size_t len, T& outValue) {
    if (len < sizeof(T)) {
        return false;
    }
    std::memcpy(&outValue, data, sizeof(T));
    return true;
}
