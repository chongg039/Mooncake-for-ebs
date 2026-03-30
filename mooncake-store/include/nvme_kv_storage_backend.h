#pragma once

#include "storage_backend.h"
#include "nvme_kv_device.h"

#include <array>
#include <atomic>
#include <unordered_map>

namespace mooncake {

/**
 * @brief Configuration for NVMe KV storage backend
 */
struct NvmeKvBackendConfig {
    // NVMe KV device path (e.g., "/dev/nvme0n1")
    std::string device_path = "/dev/nvme0n1";

    // Maximum number of KV entries
    uint64_t max_entries = 100000000;  // 100M entries

    // Eviction threshold (0.0-1.0, trigger eviction when usage exceeds this)
    double eviction_threshold = 0.9;

    // Enable Write-Ahead Logging for crash recovery
    bool wal_enabled = false;

    // WAL directory path
    std::string wal_path = "/tmp/mooncake_nvmekv_wal";

    // Number of metadata shards (power of 2 for fast modulo)
    static constexpr size_t kNumShards = 1024;

    // Enable host-side index for faster ScanMeta
    bool enable_host_index = true;

    // Queue depth for async I/O
    uint32_t queue_depth = 64;

    bool Validate() const;
    static NvmeKvBackendConfig FromEnvironment();
};

/**
 * @brief NVMe KV SSD Storage Backend
 * 
 * This backend uses NVMe KV protocol's native Key-Value interface instead of
 * LBA-based file I/O. It is designed for NVMe SSDs that support the KV command
 * set (NVMe TP 4077).
 * 
 * Key Design Principles:
 * 1. KeyID Mapping: user_key → XXH3_128bits(salt || user_key) → 16-byte KeyID
 * 2. Value Payload: [u32 key_len | u32 value_len | key | value]
 *    - Stores original key in value for collision detection
 * 3. Host Index: In-memory metadata index for fast ScanMeta operations
 * 4. Two-Phase Eviction: Consistent with BucketStorageBackend protocol
 */
class NvmeKvStorageBackend : public StorageBackendInterface {
   public:
    NvmeKvStorageBackend(const FileStorageConfig& file_storage_config,
                         const NvmeKvBackendConfig& nvme_kv_config);

    /**
     * @brief Constructor with device injection for testing
     * @param file_storage_config File storage configuration
     * @param nvme_kv_config NVMe KV backend configuration
     * @param device Pre-created device instance (for testing with mock)
     */
    NvmeKvStorageBackend(const FileStorageConfig& file_storage_config,
                         const NvmeKvBackendConfig& nvme_kv_config,
                         std::unique_ptr<NvmeKvDevice> device);

    ~NvmeKvStorageBackend();

    /**
     * @brief Initialize the NVMe KV storage backend
     */
    tl::expected<void, ErrorCode> Init() override;

    /**
     * @brief Offload objects in batches to NVMe KV SSD
     */
    tl::expected<int64_t, ErrorCode> BatchOffload(
        const std::unordered_map<std::string, std::vector<Slice>>& batch_object,
        std::function<ErrorCode(const std::vector<std::string>& keys,
                                std::vector<StorageObjectMetadata>& metadatas)>
            complete_handler,
        std::function<void(const std::vector<std::string>& evicted_keys)>
            eviction_handler = nullptr) override;

    /**
     * @brief Load multiple objects from NVMe KV SSD
     */
    tl::expected<void, ErrorCode> BatchLoad(
        std::unordered_map<std::string, Slice>& batched_slices) override;

    /**
     * @brief Check if an object exists in storage
     */
    tl::expected<bool, ErrorCode> IsExist(const std::string& key) override;

    /**
     * @brief Check if offloading is enabled (capacity check)
     */
    tl::expected<bool, ErrorCode> IsEnableOffloading() override;

    /**
     * @brief Scan all stored object metadata
     */
    tl::expected<void, ErrorCode> ScanMeta(
        const std::function<ErrorCode(
            const std::vector<std::string>& keys,
            std::vector<StorageObjectMetadata>& metadatas)>& handler) override;

    /**
     * @brief Set test failure predicate (for testing only)
     */
    void SetTestFailurePredicate(
        std::function<bool(const std::string& key)> predicate) override {
        test_failure_predicate_ = std::move(predicate);
    }

    /**
     * @brief Get device capacity information
     */
    tl::expected<void, ErrorCode> GetCapacity(
        uint64_t* total_capacity,
        uint64_t* used_capacity);

    /**
     * @brief Get the underlying device (for testing)
     */
    NvmeKvDevice* GetDevice() const { return device_.get(); }

   private:
    // On-disk record header
    struct RecordHeader {
        uint32_t key_len;
        uint32_t value_len;
        static constexpr size_t SIZE = sizeof(uint32_t) * 2;

        bool ValidateAgainstMetadata(uint32_t expected_value_len) const {
            return value_len == expected_value_len;
        }
    };

    // Host-side metadata entry for a stored object
    struct ObjectEntry {
        NvmeKvKeyId key_id;        // Device-level key
        uint32_t key_size;         // Original user key size
        uint32_t value_size;       // Value size (excluding header and key)
        uint64_t store_timestamp;  // When the object was stored

        ObjectEntry() = default;
        ObjectEntry(const NvmeKvKeyId& kid, uint32_t ks, uint32_t vs, uint64_t ts)
            : key_id(kid), key_size(ks), value_size(vs), store_timestamp(ts) {}
    };

    // Sharded metadata for reduced lock contention
    struct MetadataShard {
        mutable SharedMutex mutex;
        std::unordered_map<std::string, ObjectEntry> map;
    };

    // Hash user_key to shard index
    inline size_t ShardForKey(const std::string& key) const {
        return std::hash<std::string>{}(key) & (NvmeKvBackendConfig::kNumShards - 1);
    }

    // Generate 16-byte KeyID from user_key
    NvmeKvKeyId GenerateKeyId(const std::string& user_key) const;

    // Build value payload: [key_len | value_len | key | value]
    std::vector<char> BuildValuePayload(
        const std::string& user_key,
        const std::vector<Slice>& value_slices) const;

    // Parse value payload
    tl::expected<void, ErrorCode> ParseValuePayload(
        const char* payload,
        size_t payload_size,
        std::string* user_key,
        const char** value_data,
        size_t* value_size) const;

    // Eviction support
    struct PendingEviction {
        std::vector<std::string> keys;
        std::vector<NvmeKvKeyId> key_ids;
    };

    PendingEviction PrepareEviction(int64_t required_count);
    void FinalizeEviction(const PendingEviction& pending);

    // WAL support (optional)
    tl::expected<void, ErrorCode> WriteWalEntry(
        const std::string& key,
        const NvmeKvKeyId& key_id,
        bool is_delete);
    tl::expected<void, ErrorCode> RecoverFromWal();
    tl::expected<void, ErrorCode> CheckpointWal();

    // Host index persistence
    tl::expected<void, ErrorCode> PersistHostIndex();
    tl::expected<void, ErrorCode> LoadHostIndex();

   private:
    NvmeKvBackendConfig nvme_kv_config_;
    std::unique_ptr<NvmeKvDevice> device_;
    std::atomic<bool> initialized_{false};
    bool device_injected_ = false;  // True if device was injected (for testing)

    // Sharded metadata (host index)
    std::array<MetadataShard, NvmeKvBackendConfig::kNumShards> shards_;

    // Statistics
    std::atomic<int64_t> total_keys_{0};
    std::atomic<int64_t> total_size_{0};

    // Test-only failure injection
    std::function<bool(const std::string& key)> test_failure_predicate_;

    // Salt for key hashing
    std::array<uint8_t, 16> hash_salt_;

    // Reserved KeyID for host index persistence
    static constexpr char kHostIndexKeyPrefix[] = "__MOONCAKE_HOST_INDEX__";
};

}  // namespace mooncake
