#include "rtp/NackBuffer.h"

#include <cstdint>
#include <iostream>
#include <vector>

int main() {
    sfu::NackBuffer buffer(2);

    const std::vector<uint8_t> p1 = {1, 2, 3};
    const std::vector<uint8_t> p2 = {4, 5, 6};
    const std::vector<uint8_t> p3 = {7, 8, 9};

    buffer.Store(1, p1.data(), p1.size());
    buffer.Store(2, p2.data(), p2.size());
    buffer.Store(3, p3.data(), p3.size());

    std::vector<uint8_t> out;
    if (buffer.Get(1, &out) || !buffer.Get(2, &out) || !buffer.Get(3, &out) || buffer.Size() != 2) {
        std::cerr << "NackBuffer capacity behavior mismatch\n";
        return 1;
    }

    std::cout << "test_nack_buffer passed\n";
    return 0;
}
