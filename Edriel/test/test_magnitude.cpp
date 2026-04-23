#include <gtest/gtest.h>
#include <memory>
#include <array>
#include <cstdint>
#include <cstring>
#include <bit>

#ifdef _WIN32
#include <winsock2.h>
#else
#include <arpa/inet.h>
#endif

constexpr std::size_t TEST_BUFFER_SIZE = 1500;
using TestBuffer = std::array<char, TEST_BUFFER_SIZE>;

// Helper to create magic number in network byte order
std::array<char, 4> createMagicNumberArray(uint32_t value) {
    std::array<char, 4> arr{};
    uint32_t networkValue = htonl(value);
    std::memcpy(arr.data(), &networkValue, sizeof(networkValue));
    return arr;
}

// Standalone validation function mirroring Edriel's logic
bool hasValidMagicNumber(std::shared_ptr<TestBuffer> buffer, std::size_t length, 
                         uint32_t expectedMagic, std::size_t magicNumberSize) {
    if (length < magicNumberSize) return false;
    uint32_t receivedMagicNumber = ntohl(*reinterpret_cast<const uint32_t*>(buffer->data()));
    return receivedMagicNumber == expectedMagic;
}

TEST(TestMagicNumber, ValidMagicNumber) {
    constexpr uint32_t MAGIC_NUMBER = 0xED75E1ED;
    constexpr std::size_t MAGIC_NUMBER_SIZE = sizeof(MAGIC_NUMBER);
    
    auto buffer = std::make_shared<TestBuffer>();
    
    // Create buffer with valid magic number
    auto magicArray = createMagicNumberArray(MAGIC_NUMBER);
    std::memcpy(buffer->data(), magicArray.data(), MAGIC_NUMBER_SIZE);
    
    // Add some data after magic number
    for (std::size_t i = MAGIC_NUMBER_SIZE; i < TEST_BUFFER_SIZE; ++i) {
        buffer->data()[i] = static_cast<char>(i % 256);
    }
    
    EXPECT_TRUE(hasValidMagicNumber(buffer, TEST_BUFFER_SIZE, MAGIC_NUMBER, MAGIC_NUMBER_SIZE));
}

TEST(TestMagicNumber, InvalidMagicNumber) {
    constexpr uint32_t MAGIC_NUMBER = 0xED75E1ED;
    constexpr uint32_t INVALID_MAGIC_NUMBER = 0x12345678;
    constexpr std::size_t MAGIC_NUMBER_SIZE = sizeof(MAGIC_NUMBER);
    
    auto buffer = std::make_shared<TestBuffer>();
    
    // Create buffer with invalid magic number
    auto magicArray = createMagicNumberArray(INVALID_MAGIC_NUMBER);
    std::memcpy(buffer->data(), magicArray.data(), MAGIC_NUMBER_SIZE);
    
    EXPECT_FALSE(hasValidMagicNumber(buffer, TEST_BUFFER_SIZE, MAGIC_NUMBER, MAGIC_NUMBER_SIZE));
}

TEST(TestMagicNumber, BufferTooSmall) {
    constexpr uint32_t MAGIC_NUMBER = 0xED75E1ED;
    constexpr std::size_t MAGIC_NUMBER_SIZE = sizeof(MAGIC_NUMBER);
    
    auto buffer = std::make_shared<TestBuffer>();
    std::memcpy(buffer->data(), "test", 4);
    
    // Pass a length smaller than magic number size
    EXPECT_FALSE(hasValidMagicNumber(buffer, 2, MAGIC_NUMBER, MAGIC_NUMBER_SIZE));
}

TEST(TestMagicNumber, ZeroLength) {
    constexpr uint32_t MAGIC_NUMBER = 0xED75E1ED;
    constexpr std::size_t MAGIC_NUMBER_SIZE = sizeof(MAGIC_NUMBER);
    
    auto buffer = std::make_shared<TestBuffer>();
    
    EXPECT_FALSE(hasValidMagicNumber(buffer, 0, MAGIC_NUMBER, MAGIC_NUMBER_SIZE));
}
