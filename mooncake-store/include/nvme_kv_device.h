#pragma once

#include <atomic>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>
#include <functional>

#include <ylt/util/tl/expected.hpp>

#include "types.h"

namespace mooncake {

/**
 * @brief NVMe KV command opcodes (NVMe TP 4077)
 */
enum class NvmeKvOpcode : uint8_t {
    STORE = 0x01,     // Store key-value pair
    RETRIEVE = 0x02,  // Retrieve value by key
    DELETE = 0x10,    // Delete key-value pair
    EXIST = 0x14,     // Check if key exists
    LIST = 0x06,      // List keys (optional)
};

/**
 * @brief NVMe KV key structure (16 bytes max by standard, extensible)
 * 
 * For keys longer than 16 bytes, we use a hash-based KeyID mapping:
 *   KeyID = XXH128(salt || user_key)
 * 
 * Hash Strategy:
 * - Uses XXHash3 128-bit (XXH128) for high-quality distribution
 * - Salt ensures different backends generate different KeyIDs for same key
 * - Collision probability: ~1/2^64 for birthday problem with 2^32 keys
 * 
 * The actual user_key is stored in the value payload for collision detection.
 */
struct NvmeKvKeyId {
    static constexpr size_t SIZE = 16;
    uint8_t data[SIZE];

    NvmeKvKeyId() { memset(data, 0, SIZE); }

    /**
     * @brief Construct KeyID from user_key using simple hash (no salt)
     * @note Prefer using the salted version for production
     */
    explicit NvmeKvKeyId(const std::string& user_key);

    /**
     * @brief Construct KeyID from user_key with salt (recommended)
     * @param salt 16-byte salt for domain separation
     * @param user_key The application-level key
     * 
     * Uses XXH128(salt || user_key) for high-quality 128-bit hash
     */
    NvmeKvKeyId(const uint8_t* salt, size_t salt_len, const std::string& user_key);

    /**
     * @brief Construct KeyID from raw 16-byte data
     */
    static NvmeKvKeyId FromRawBytes(const uint8_t* bytes);

    bool operator==(const NvmeKvKeyId& other) const {
        return memcmp(data, other.data, SIZE) == 0;
    }

    bool operator!=(const NvmeKvKeyId& other) const {
        return !(*this == other);
    }

    /**
     * @brief Get hash value for use in std::unordered_map
     */
    size_t hash() const;

    std::string to_hex_string() const;
};

/**
 * @brief Configuration for NVMe KV device
 */
struct NvmeKvDeviceConfig {
    std::string device_path;         // e.g., "/dev/nvme0n1"
    uint32_t queue_depth = 64;       // I/O queue depth
    uint32_t max_key_size = 16;      // Max key size supported by device
    uint32_t max_value_size = 2 * 1024 * 1024;  // Max value size (2MB default)
    bool enable_async = true;        // Enable async I/O
    uint32_t io_threads = 4;         // Number of I/O threads for async mode

    bool Validate() const;
    static NvmeKvDeviceConfig FromEnvironment();
};

/**
 * @brief Result of a single KV operation
 */
struct NvmeKvResult {
    ErrorCode error_code;
    size_t bytes_transferred;

    bool is_ok() const { return error_code == ErrorCode::OK; }
};

/**
 * @brief Batch operation descriptor for async I/O
 */
struct NvmeKvBatchOp {
    NvmeKvOpcode opcode;
    NvmeKvKeyId key_id;
    void* buffer;           // Data buffer (for STORE/RETRIEVE)
    size_t buffer_size;     // Buffer size
    size_t value_offset;    // Offset within value (for partial read/write)
    size_t value_length;    // Length to read/write

    // Completion callback
    std::function<void(const NvmeKvResult&)> callback;
};

/**
 * @brief Abstract interface for NVMe KV device operations
 * 
 * This class encapsulates direct NVMe KV protocol commands (non-LBA semantics).
 * Implementations can use:
 *   - Linux NVMe ioctl (nvme-cli style)
 *   - SPDK for userspace drivers
 *   - Vendor-specific libraries (Samsung KV SSD SDK, etc.)
 */
class NvmeKvDevice {
   public:
    virtual ~NvmeKvDevice() = default;

    /**
     * @brief Initialize the device
     * @return tl::expected<void, ErrorCode> indicating operation status
     */
    virtual tl::expected<void, ErrorCode> Init() = 0;

    /**
     * @brief Close the device and release resources
     */
    virtual void Close() = 0;

    /**
     * @brief Store a key-value pair
     * @param key_id The 16-byte key identifier
     * @param value Pointer to value data
     * @param value_size Size of value in bytes
     * @return tl::expected<void, ErrorCode> indicating operation status
     */
    virtual tl::expected<void, ErrorCode> Store(
        const NvmeKvKeyId& key_id,
        const void* value,
        size_t value_size) = 0;

    /**
     * @brief Retrieve a value by key
     * @param key_id The 16-byte key identifier
     * @param buffer Output buffer for value data
     * @param buffer_size Size of output buffer
     * @param[out] actual_size Actual size of value read
     * @return tl::expected<void, ErrorCode> indicating operation status
     */
    virtual tl::expected<void, ErrorCode> Retrieve(
        const NvmeKvKeyId& key_id,
        void* buffer,
        size_t buffer_size,
        size_t* actual_size) = 0;

    /**
     * @brief Delete a key-value pair
     * @param key_id The 16-byte key identifier
     * @return tl::expected<void, ErrorCode> indicating operation status
     */
    virtual tl::expected<void, ErrorCode> Delete(const NvmeKvKeyId& key_id) = 0;

    /**
     * @brief Check if a key exists
     * @param key_id The 16-byte key identifier
     * @return tl::expected<bool, ErrorCode> true if exists, false otherwise
     */
    virtual tl::expected<bool, ErrorCode> Exist(const NvmeKvKeyId& key_id) = 0;

    /**
     * @brief Batch submit operations (async mode)
     * @param ops Vector of batch operations
     * @return tl::expected<void, ErrorCode> indicating submission status
     */
    virtual tl::expected<void, ErrorCode> BatchSubmit(
        std::vector<NvmeKvBatchOp>& ops) = 0;

    /**
     * @brief Wait for all submitted operations to complete
     * @param timeout_ms Timeout in milliseconds (0 = wait forever)
     * @return Number of completed operations, or -1 on error
     */
    virtual int WaitCompletion(uint32_t timeout_ms = 0) = 0;

    /**
     * @brief Get device capacity information
     * @param[out] total_capacity Total capacity in bytes
     * @param[out] used_capacity Used capacity in bytes
     * @return tl::expected<void, ErrorCode> indicating operation status
     */
    virtual tl::expected<void, ErrorCode> GetCapacity(
        uint64_t* total_capacity,
        uint64_t* used_capacity) = 0;

    /**
     * @brief Create an NVMe KV device instance
     * @param config Device configuration
     * @return Unique pointer to device instance
     */
    static std::unique_ptr<NvmeKvDevice> Create(const NvmeKvDeviceConfig& config);
};

/**
 * @brief Linux ioctl-based NVMe KV device implementation
 * 
 * Uses standard Linux NVMe passthrough ioctl for KV commands.
 * Requires kernel support for NVMe KV command set.
 */
class LinuxNvmeKvDevice : public NvmeKvDevice {
   public:
    explicit LinuxNvmeKvDevice(const NvmeKvDeviceConfig& config);
    ~LinuxNvmeKvDevice() override;

    tl::expected<void, ErrorCode> Init() override;
    void Close() override;

    tl::expected<void, ErrorCode> Store(
        const NvmeKvKeyId& key_id,
        const void* value,
        size_t value_size) override;

    tl::expected<void, ErrorCode> Retrieve(
        const NvmeKvKeyId& key_id,
        void* buffer,
        size_t buffer_size,
        size_t* actual_size) override;

    tl::expected<void, ErrorCode> Delete(const NvmeKvKeyId& key_id) override;

    tl::expected<bool, ErrorCode> Exist(const NvmeKvKeyId& key_id) override;

    tl::expected<void, ErrorCode> BatchSubmit(
        std::vector<NvmeKvBatchOp>& ops) override;

    int WaitCompletion(uint32_t timeout_ms = 0) override;

    tl::expected<void, ErrorCode> GetCapacity(
        uint64_t* total_capacity,
        uint64_t* used_capacity) override;

   private:
    NvmeKvDeviceConfig config_;
    int fd_ = -1;  // Device file descriptor
    std::atomic<bool> initialized_{false};

    // Async I/O support
    struct AsyncContext;
    std::unique_ptr<AsyncContext> async_ctx_;

    // Helper methods
    tl::expected<void, ErrorCode> SendKvCommand(
        NvmeKvOpcode opcode,
        const NvmeKvKeyId& key_id,
        void* buffer,
        size_t buffer_size,
        size_t* bytes_transferred);
};

}  // namespace mooncake
