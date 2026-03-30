#include "nvme_kv_storage_backend.h"

#include <chrono>
#include <filesystem>
#include <fstream>
#include <random>

#include <glog/logging.h>

#include "mutex.h"
#include "utils.h"

namespace mooncake {

// ============================================================================
// NvmeKvBackendConfig Implementation
// ============================================================================

bool NvmeKvBackendConfig::Validate() const {
    if (device_path.empty()) {
        LOG(ERROR) << "NvmeKvBackendConfig: device_path is empty";
        return false;
    }
    if (max_entries == 0) {
        LOG(ERROR) << "NvmeKvBackendConfig: max_entries must be > 0";
        return false;
    }
    if (eviction_threshold < 0.0 || eviction_threshold > 1.0) {
        LOG(ERROR) << "NvmeKvBackendConfig: eviction_threshold must be in [0.0, 1.0]";
        return false;
    }
    if (wal_enabled && wal_path.empty()) {
        LOG(ERROR) << "NvmeKvBackendConfig: wal_path required when WAL is enabled";
        return false;
    }
    return true;
}

NvmeKvBackendConfig NvmeKvBackendConfig::FromEnvironment() {
    NvmeKvBackendConfig config;

    config.device_path = GetEnvStringOr(
        "MOONCAKE_NVME_KV_DEVICE_PATH", config.device_path);
    config.max_entries = GetEnvOr<uint64_t>(
        "MOONCAKE_NVME_KV_MAX_ENTRIES", config.max_entries);
    config.eviction_threshold = GetEnvOr<double>(
        "MOONCAKE_NVME_KV_EVICTION_THRESHOLD", config.eviction_threshold);
    config.wal_enabled = GetEnvOr<bool>(
        "MOONCAKE_NVME_KV_WAL_ENABLED", config.wal_enabled);
    config.wal_path = GetEnvStringOr(
        "MOONCAKE_NVME_KV_WAL_PATH", config.wal_path);
    config.enable_host_index = GetEnvOr<bool>(
        "MOONCAKE_NVME_KV_ENABLE_HOST_INDEX", config.enable_host_index);
    config.queue_depth = GetEnvOr<uint32_t>(
        "MOONCAKE_NVME_KV_QUEUE_DEPTH", config.queue_depth);

    return config;
}

// ============================================================================
// NvmeKvStorageBackend Implementation
// ============================================================================

NvmeKvStorageBackend::NvmeKvStorageBackend(
    const FileStorageConfig& file_storage_config,
    const NvmeKvBackendConfig& nvme_kv_config)
    : StorageBackendInterface(file_storage_config),
      nvme_kv_config_(nvme_kv_config),
      device_injected_(false) {
    
    // Initialize hash salt with random bytes
    std::random_device rd;
    std::mt19937_64 gen(rd());
    for (size_t i = 0; i < hash_salt_.size(); ++i) {
        hash_salt_[i] = static_cast<uint8_t>(gen() & 0xFF);
    }
}

NvmeKvStorageBackend::NvmeKvStorageBackend(
    const FileStorageConfig& file_storage_config,
    const NvmeKvBackendConfig& nvme_kv_config,
    std::unique_ptr<NvmeKvDevice> device)
    : StorageBackendInterface(file_storage_config),
      nvme_kv_config_(nvme_kv_config),
      device_(std::move(device)),
      device_injected_(true) {
    
    // Initialize hash salt with random bytes
    std::random_device rd;
    std::mt19937_64 gen(rd());
    for (size_t i = 0; i < hash_salt_.size(); ++i) {
        hash_salt_[i] = static_cast<uint8_t>(gen() & 0xFF);
    }
}

NvmeKvStorageBackend::~NvmeKvStorageBackend() {
    if (device_) {
        // Persist host index before shutdown
        auto result = PersistHostIndex();
        if (!result) {
            LOG(WARNING) << "Failed to persist host index on shutdown";
        }
        device_->Close();
    }
}

tl::expected<void, ErrorCode> NvmeKvStorageBackend::Init() {
    if (initialized_.load(std::memory_order_acquire)) {
        return {};
    }

    if (!nvme_kv_config_.Validate()) {
        return tl::make_unexpected(ErrorCode::INVALID_PARAMS);
    }

    // If device was injected (for testing), skip device creation
    if (!device_injected_) {
        // Create device configuration
        NvmeKvDeviceConfig device_config;
        device_config.device_path = nvme_kv_config_.device_path;
        device_config.queue_depth = nvme_kv_config_.queue_depth;

        // Create and initialize device
        device_ = NvmeKvDevice::Create(device_config);
    }

    auto init_result = device_->Init();
    if (!init_result) {
        LOG(ERROR) << "Failed to initialize NVMe KV device: "
                   << nvme_kv_config_.device_path;
        return init_result;
    }

    // Load host index from device (if exists)
    if (nvme_kv_config_.enable_host_index) {
        auto load_result = LoadHostIndex();
        if (!load_result) {
            LOG(INFO) << "No existing host index found, starting fresh";
        } else {
            LOG(INFO) << "Host index loaded, total_keys=" << total_keys_.load();
        }
    }

    // Recover from WAL if enabled
    if (nvme_kv_config_.wal_enabled) {
        auto wal_result = RecoverFromWal();
        if (!wal_result) {
            LOG(WARNING) << "WAL recovery failed, continuing without WAL data";
        }
    }

    LOG(INFO) << "NVMe KV storage backend initialized: "
              << "device=" << nvme_kv_config_.device_path
              << ", max_entries=" << nvme_kv_config_.max_entries
              << ", eviction_threshold=" << nvme_kv_config_.eviction_threshold;

    initialized_.store(true, std::memory_order_release);
    return {};
}

NvmeKvKeyId NvmeKvStorageBackend::GenerateKeyId(const std::string& user_key) const {
    // Generate KeyID using XXH3_128bits with salt
    // 
    // KeyID = XXH3_128bits(salt || user_key)
    // 
    // The salt provides domain separation:
    // - Different backend instances generate different KeyIDs
    // - Prevents cross-instance key collision on shared devices
    //
    // Hash Properties:
    // - XXH3 provides excellent avalanche and distribution
    // - 128-bit output fits perfectly in NVMe KV 16-byte KeyID
    // - ~30 GB/s throughput on modern CPUs
    
    return NvmeKvKeyId(hash_salt_.data(), hash_salt_.size(), user_key);
}

std::vector<char> NvmeKvStorageBackend::BuildValuePayload(
    const std::string& user_key,
    const std::vector<Slice>& value_slices) const {
    
    // Calculate total value size
    size_t value_size = 0;
    for (const auto& slice : value_slices) {
        value_size += slice.size;
    }

    // Payload format: [u32 key_len | u32 value_len | key | value]
    size_t payload_size = RecordHeader::SIZE + user_key.size() + value_size;
    std::vector<char> payload(payload_size);

    // Write header
    RecordHeader header;
    header.key_len = static_cast<uint32_t>(user_key.size());
    header.value_len = static_cast<uint32_t>(value_size);
    memcpy(payload.data(), &header, RecordHeader::SIZE);

    // Write key
    memcpy(payload.data() + RecordHeader::SIZE, user_key.data(), user_key.size());

    // Write value slices
    size_t offset = RecordHeader::SIZE + user_key.size();
    for (const auto& slice : value_slices) {
        memcpy(payload.data() + offset, slice.ptr, slice.size);
        offset += slice.size;
    }

    return payload;
}

tl::expected<void, ErrorCode> NvmeKvStorageBackend::ParseValuePayload(
    const char* payload,
    size_t payload_size,
    std::string* user_key,
    const char** value_data,
    size_t* value_size) const {
    
    if (payload_size < RecordHeader::SIZE) {
        return tl::make_unexpected(ErrorCode::FILE_READ_FAIL);
    }

    RecordHeader header;
    memcpy(&header, payload, RecordHeader::SIZE);

    size_t expected_size = RecordHeader::SIZE + header.key_len + header.value_len;
    if (payload_size < expected_size) {
        LOG(ERROR) << "Payload size mismatch: expected " << expected_size
                   << ", got " << payload_size;
        return tl::make_unexpected(ErrorCode::FILE_READ_FAIL);
    }

    if (user_key) {
        *user_key = std::string(payload + RecordHeader::SIZE, header.key_len);
    }
    if (value_data) {
        *value_data = payload + RecordHeader::SIZE + header.key_len;
    }
    if (value_size) {
        *value_size = header.value_len;
    }

    return {};
}

tl::expected<int64_t, ErrorCode> NvmeKvStorageBackend::BatchOffload(
    const std::unordered_map<std::string, std::vector<Slice>>& batch_object,
    std::function<ErrorCode(const std::vector<std::string>& keys,
                            std::vector<StorageObjectMetadata>& metadatas)>
        complete_handler,
    std::function<void(const std::vector<std::string>& evicted_keys)>
        eviction_handler) {
    
    if (!initialized_.load(std::memory_order_acquire)) {
        LOG(ERROR) << "NVMe KV storage backend not initialized";
        return tl::make_unexpected(ErrorCode::INTERNAL_ERROR);
    }

    if (batch_object.empty()) {
        return 0;
    }

    // Check capacity and prepare eviction if needed
    int64_t required_count = static_cast<int64_t>(batch_object.size());
    PendingEviction pending = PrepareEviction(required_count);

    // Notify master about evicted keys before storing new ones
    if (eviction_handler && !pending.keys.empty()) {
        eviction_handler(pending.keys);
    }

    // Finalize eviction (delete from device)
    FinalizeEviction(pending);

    // Store each key-value pair
    std::vector<std::string> stored_keys;
    std::vector<StorageObjectMetadata> stored_metadatas;
    stored_keys.reserve(batch_object.size());
    stored_metadatas.reserve(batch_object.size());

    int64_t success_count = 0;
    auto now_ns = std::chrono::duration_cast<std::chrono::nanoseconds>(
        std::chrono::system_clock::now().time_since_epoch()).count();

    for (const auto& [key, slices] : batch_object) {
        // Check test failure injection
        if (test_failure_predicate_ && test_failure_predicate_(key)) {
            LOG(WARNING) << "Test failure injected for key: " << key;
            continue;
        }

        // Generate KeyID
        NvmeKvKeyId key_id = GenerateKeyId(key);

        // Build value payload
        std::vector<char> payload = BuildValuePayload(key, slices);

        // Store to device
        auto store_result = device_->Store(key_id, payload.data(), payload.size());
        if (!store_result) {
            LOG(ERROR) << "Failed to store key: " << key
                       << ", error: " << store_result.error();
            continue;
        }

        // Calculate value size (excluding header and key)
        size_t value_size = 0;
        for (const auto& slice : slices) {
            value_size += slice.size;
        }

        // Update host index
        size_t shard_idx = ShardForKey(key);
        {
            SharedMutexLocker lock(&shards_[shard_idx].mutex);
            auto [it, inserted] = shards_[shard_idx].map.try_emplace(
                key, 
                ObjectEntry(key_id, 
                           static_cast<uint32_t>(key.size()),
                           static_cast<uint32_t>(value_size),
                           now_ns));
            
            if (!inserted) {
                // Key already exists, update it
                it->second = ObjectEntry(key_id,
                                        static_cast<uint32_t>(key.size()),
                                        static_cast<uint32_t>(value_size),
                                        now_ns);
            } else {
                total_keys_.fetch_add(1, std::memory_order_relaxed);
            }
        }

        total_size_.fetch_add(static_cast<int64_t>(payload.size()),
                              std::memory_order_relaxed);

        // Build metadata for complete_handler
        stored_keys.push_back(key);
        StorageObjectMetadata meta;
        meta.data_size = static_cast<int64_t>(value_size);
        meta.key_size = static_cast<int64_t>(key.size());
        stored_metadatas.push_back(meta);

        ++success_count;

        // Write WAL entry if enabled
        if (nvme_kv_config_.wal_enabled) {
            WriteWalEntry(key, key_id, false);
        }
    }

    // Call complete handler
    if (complete_handler && !stored_keys.empty()) {
        auto error = complete_handler(stored_keys, stored_metadatas);
        if (error != ErrorCode::OK) {
            LOG(ERROR) << "Complete handler failed: " << error;
            return tl::make_unexpected(error);
        }
    }

    return success_count;
}

tl::expected<void, ErrorCode> NvmeKvStorageBackend::BatchLoad(
    std::unordered_map<std::string, Slice>& batched_slices) {
    
    if (!initialized_.load(std::memory_order_acquire)) {
        return tl::make_unexpected(ErrorCode::INTERNAL_ERROR);
    }

    for (auto& [key, slice] : batched_slices) {
        // Lookup in host index
        size_t shard_idx = ShardForKey(key);
        ObjectEntry entry;
        {
            SharedMutexLocker lock(&shards_[shard_idx].mutex, shared_lock);
            auto it = shards_[shard_idx].map.find(key);
            if (it == shards_[shard_idx].map.end()) {
                LOG(ERROR) << "Key not found in host index: " << key;
                return tl::make_unexpected(ErrorCode::OBJECT_NOT_FOUND);
            }
            entry = it->second;
        }

        // Calculate expected payload size
        size_t payload_size = RecordHeader::SIZE + entry.key_size + entry.value_size;
        
        // Allocate temporary buffer for full payload
        std::vector<char> payload_buffer(payload_size);

        // Retrieve from device
        size_t actual_size = 0;
        auto retrieve_result = device_->Retrieve(
            entry.key_id,
            payload_buffer.data(),
            payload_buffer.size(),
            &actual_size);
        
        if (!retrieve_result) {
            LOG(ERROR) << "Failed to retrieve key: " << key
                       << ", error: " << retrieve_result.error();
            return retrieve_result;
        }

        // Parse payload and verify key
        std::string stored_key;
        const char* value_data = nullptr;
        size_t value_size = 0;
        auto parse_result = ParseValuePayload(
            payload_buffer.data(), actual_size,
            &stored_key, &value_data, &value_size);
        
        if (!parse_result) {
            return parse_result;
        }

        // Verify key matches (collision detection)
        if (stored_key != key) {
            LOG(ERROR) << "Key collision detected: expected=" << key
                       << ", stored=" << stored_key;
            return tl::make_unexpected(ErrorCode::INTERNAL_ERROR);
        }

        // Copy value to output slice
        if (slice.size < value_size) {
            LOG(ERROR) << "Output buffer too small: " << slice.size
                       << " < " << value_size;
            return tl::make_unexpected(ErrorCode::INVALID_PARAMS);
        }

        memcpy(slice.ptr, value_data, value_size);
        slice.size = value_size;  // Update to actual size
    }

    return {};
}

tl::expected<bool, ErrorCode> NvmeKvStorageBackend::IsExist(
    const std::string& key) {
    
    if (!initialized_.load(std::memory_order_acquire)) {
        return tl::make_unexpected(ErrorCode::INTERNAL_ERROR);
    }

    // Check host index first (faster)
    size_t shard_idx = ShardForKey(key);
    {
        SharedMutexLocker lock(&shards_[shard_idx].mutex, shared_lock);
        if (shards_[shard_idx].map.find(key) != shards_[shard_idx].map.end()) {
            return true;
        }
    }

    return false;
}

tl::expected<bool, ErrorCode> NvmeKvStorageBackend::IsEnableOffloading() {
    if (!initialized_.load(std::memory_order_acquire)) {
        return tl::make_unexpected(ErrorCode::INTERNAL_ERROR);
    }

    int64_t current_keys = total_keys_.load(std::memory_order_relaxed);
    int64_t max_keys = static_cast<int64_t>(nvme_kv_config_.max_entries);
    
    // Check against configured limits
    bool within_key_limit = current_keys < 
        static_cast<int64_t>(file_storage_config_.total_keys_limit);
    bool within_entry_limit = current_keys < max_keys;

    // Check against eviction threshold
    double usage_ratio = static_cast<double>(current_keys) / max_keys;
    bool within_threshold = usage_ratio < nvme_kv_config_.eviction_threshold;

    return within_key_limit && within_entry_limit && within_threshold;
}

tl::expected<void, ErrorCode> NvmeKvStorageBackend::ScanMeta(
    const std::function<ErrorCode(
        const std::vector<std::string>& keys,
        std::vector<StorageObjectMetadata>& metadatas)>& handler) {
    
    if (!initialized_.load(std::memory_order_acquire)) {
        return tl::make_unexpected(ErrorCode::INTERNAL_ERROR);
    }

    // Use host index for efficient scanning
    int64_t batch_size = file_storage_config_.scanmeta_iterator_keys_limit;
    std::vector<std::string> keys_batch;
    std::vector<StorageObjectMetadata> metadatas_batch;
    keys_batch.reserve(batch_size);
    metadatas_batch.reserve(batch_size);

    for (size_t shard_idx = 0; shard_idx < NvmeKvBackendConfig::kNumShards; ++shard_idx) {
        // Collect all entries from this shard under lock
        std::vector<std::pair<std::string, ObjectEntry>> shard_entries;
        {
            SharedMutexLocker lock(&shards_[shard_idx].mutex, shared_lock);
            for (const auto& [key, entry] : shards_[shard_idx].map) {
                shard_entries.emplace_back(key, entry);
            }
        }
        
        // Process entries outside lock
        for (const auto& [key, entry] : shard_entries) {
            keys_batch.push_back(key);
            
            StorageObjectMetadata meta;
            meta.data_size = entry.value_size;
            meta.key_size = entry.key_size;
            metadatas_batch.push_back(meta);

            if (static_cast<int64_t>(keys_batch.size()) >= batch_size) {
                auto error = handler(keys_batch, metadatas_batch);
                if (error != ErrorCode::OK) {
                    return tl::make_unexpected(error);
                }
                
                keys_batch.clear();
                metadatas_batch.clear();
            }
        }
    }

    // Handle remaining items
    if (!keys_batch.empty()) {
        auto error = handler(keys_batch, metadatas_batch);
        if (error != ErrorCode::OK) {
            return tl::make_unexpected(error);
        }
    }

    return {};
}

tl::expected<void, ErrorCode> NvmeKvStorageBackend::GetCapacity(
    uint64_t* total_capacity,
    uint64_t* used_capacity) {
    
    if (!initialized_.load(std::memory_order_acquire)) {
        return tl::make_unexpected(ErrorCode::INTERNAL_ERROR);
    }

    return device_->GetCapacity(total_capacity, used_capacity);
}

// ============================================================================
// Eviction Support
// ============================================================================

NvmeKvStorageBackend::PendingEviction NvmeKvStorageBackend::PrepareEviction(
    int64_t required_count) {
    
    PendingEviction result;

    int64_t current_keys = total_keys_.load(std::memory_order_relaxed);
    int64_t max_keys = static_cast<int64_t>(nvme_kv_config_.max_entries);
    
    // Check if eviction is needed
    if (current_keys + required_count <= max_keys) {
        return result;
    }

    // Calculate how many keys to evict
    int64_t evict_count = current_keys + required_count - max_keys;
    evict_count = std::max(evict_count, 
                          static_cast<int64_t>(max_keys * 0.1));  // Evict at least 10%

    LOG(INFO) << "[NvmeKv Evict] triggered: current=" << current_keys
              << ", required=" << required_count
              << ", max=" << max_keys
              << ", evicting=" << evict_count;

    // Select candidates (FIFO based on store_timestamp)
    // Using simple approach: iterate all shards and collect oldest entries
    std::vector<std::pair<uint64_t, std::pair<size_t, std::string>>> candidates;
    
    for (size_t shard_idx = 0; shard_idx < NvmeKvBackendConfig::kNumShards; ++shard_idx) {
        SharedMutexLocker lock(&shards_[shard_idx].mutex, shared_lock);
        for (const auto& [key, entry] : shards_[shard_idx].map) {
            candidates.emplace_back(entry.store_timestamp, 
                                   std::make_pair(shard_idx, key));
        }
    }

    // Sort by timestamp (oldest first)
    std::sort(candidates.begin(), candidates.end());

    // Collect eviction targets
    int64_t collected = 0;
    for (const auto& [ts, shard_key] : candidates) {
        if (collected >= evict_count) break;

        size_t shard_idx = shard_key.first;
        const std::string& key = shard_key.second;

        SharedMutexLocker lock(&shards_[shard_idx].mutex);
        auto it = shards_[shard_idx].map.find(key);
        if (it != shards_[shard_idx].map.end()) {
            result.keys.push_back(key);
            result.key_ids.push_back(it->second.key_id);
            
            // Remove from host index
            total_size_.fetch_sub(
                RecordHeader::SIZE + it->second.key_size + it->second.value_size,
                std::memory_order_relaxed);
            total_keys_.fetch_sub(1, std::memory_order_relaxed);
            shards_[shard_idx].map.erase(it);
            
            ++collected;
        }
    }

    LOG(INFO) << "[NvmeKv Evict] prepared: " << result.keys.size() << " keys";
    return result;
}

void NvmeKvStorageBackend::FinalizeEviction(const PendingEviction& pending) {
    if (pending.keys.empty()) {
        return;
    }

    // Delete from device
    for (size_t i = 0; i < pending.key_ids.size(); ++i) {
        auto delete_result = device_->Delete(pending.key_ids[i]);
        if (!delete_result) {
            LOG(WARNING) << "Failed to delete key from device: "
                        << pending.keys[i];
        }

        // Write WAL entry if enabled
        if (nvme_kv_config_.wal_enabled) {
            WriteWalEntry(pending.keys[i], pending.key_ids[i], true);
        }
    }

    LOG(INFO) << "[NvmeKv Evict] finalized: deleted " 
              << pending.keys.size() << " keys from device";
}

// ============================================================================
// WAL Support (Stub - implement based on requirements)
// ============================================================================

tl::expected<void, ErrorCode> NvmeKvStorageBackend::WriteWalEntry(
    const std::string& key,
    const NvmeKvKeyId& key_id,
    bool is_delete) {
    // TODO: Implement WAL write
    return {};
}

tl::expected<void, ErrorCode> NvmeKvStorageBackend::RecoverFromWal() {
    // TODO: Implement WAL recovery
    return {};
}

tl::expected<void, ErrorCode> NvmeKvStorageBackend::CheckpointWal() {
    // TODO: Implement WAL checkpoint
    return {};
}

// ============================================================================
// Host Index Persistence
// ============================================================================

tl::expected<void, ErrorCode> NvmeKvStorageBackend::PersistHostIndex() {
    // TODO: Serialize host index and store to device using reserved key
    // This can use a simple binary format or protobuf
    return {};
}

tl::expected<void, ErrorCode> NvmeKvStorageBackend::LoadHostIndex() {
    // TODO: Load host index from device
    return tl::make_unexpected(ErrorCode::OBJECT_NOT_FOUND);
}

}  // namespace mooncake
