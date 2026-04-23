#include <gtest/gtest.h>
#include <memory>
#include <array>
#include <cstdint>

// Helper functions for magic number tests
uint32_t ntohl(uint32_t hostlong) {
    return __builtin_bswap32(hostlong);
}

std::array<char, 4> createMagicNumberArray(uint32_t value) {
    std::array<char, 4> arr{};
    uint32_t networkValue = htonl(value);
    memcpy(arr.data(), &networkValue, sizeof(networkValue));
    return arr;
}

bool hasValidMagicNumber(std::shared_ptr<std::array<char, 1500>> buffer, std::size_t length, 
                         uint32_t magicNumber, std::size_t magicNumberSize) {
    if (length < magicNumberSize) return false;
    auto rawBytes = *reinterpret_cast<const std::array<char, magicNumberSize>*>(buffer->data());
    uint32_t receivedMagicNumber = ntohl(std::bit_cast<uint32_t>(rawBytes));
    return receivedMagicNumber == magicNumber;
}

TEST(TestMagicNumber, ValidMagicNumber) {
    constexpr uint32_t MAGIC_NUMBER = 0xED75E1ED;
    constexpr std::size_t MAGIC_NUMBER_SIZE = sizeof(MAGIC_NUMBER);
    constexpr std::size_t BUFFER_SIZE = 1500;
    
    std::shared_ptr<std::array<char, BUFFER_SIZE>> buffer = 
        std::make_shared<std::array<char, BUFFER_SIZE>>();
    
    // Create buffer with valid magic number
    auto magicArray = createMagicNumberArray(MAGIC_NUMBER);
    memcpy(buffer->data(), magicArray.data(), MAGIC_NUMBER_SIZE);
    
    // Add some data after magic number
    for (std::size_t i = MAGIC_NUMBER_SIZE; i < BUFFER_SIZE; ++i) {
        buffer->data()[i] = static_cast<char>(i % 256);
    }
    
    std::size_t length = BUFFER_SIZE;
    
    EXPECT_TRUE(hasValidMagicNumber(buffer, length, MAGIC_NUMBER, MAGIC_NUMBER_SIZE));
}

TEST(TestMagicNumber, InvalidMagicNumber) {
    constexpr uint32_t MAGIC_NUMBER = 0xED75E1ED;
    constexpr uint32_t INVALID_MAGIC_NUMBER = 0x12345678;
    constexpr std::size_t MAGIC_NUMBER_SIZE = sizeof(MAGIC_NUMBER);
    constexpr std::size_t BUFFER_SIZE = 1500;
    
    std::shared_ptr<std::array<char, BUFFER_SIZE>> buffer = 
        std::make_shared<std::array<char, BUFFER_SIZE>>();
    
    // Create buffer with invalid magic number
    auto magicArray = createMagicNumberArray(INVALID_MAGIC_NUMBER);
    memcpy(buffer->data(), magicArray.data(), MAGIC_NUMBER_SIZE);
    
    std::size_t length = BUFFER_SIZE;
    
    EXPECT_FALSE(hasValidMagicNumber(buffer, length, MAGIC_NUMBER, MAGIC_NUMBER_SIZE));
}

TEST(TestMagicNumber, BufferTooSmall) {
    constexpr uint32_t MAGIC_NUMBER = 0xED75E1ED;
    constexpr std::size_t MAGIC_NUMBER_SIZE = sizeof(MAGIC_NUMBER);
    
    // Create small buffer (less than magic number size)
    std::shared_ptr<std::array<char, 10>> smallBuffer = 
        std::make_shared<std::array<char, 10>>();
    
    memcpy(smallBuffer->data(), "test", 4);
    
    std::size_t length = 10;
    
    EXPECT_FALSE(hasValidMagicNumber(smallBuffer, length, MAGIC_NUMBER, MAGIC_NUMBER_SIZE));
}

TEST(TestMagicNumber, ZeroLength) {
    constexpr uint32_t MAGIC_NUMBER = 0xED75E1ED;
    constexpr std::size_t MAGIC_NUMBER_SIZE = sizeof(MAGIC_NUMBER);
    constexpr std::size_t BUFFER_SIZE = 1500;
    
    std::shared_ptr<std::array<char, BUFFER_SIZE>> buffer = 
        std::make_shared<std::array<char, BUFFER_SIZE>>();
    
    std::size_t length = 0;
    
    EXPECT_FALSE(hasValidMagicNumber(buffer, length, MAGIC_NUMBER, MAGIC_NUMBER_SIZE));
}
