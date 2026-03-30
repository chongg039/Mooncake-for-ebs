# NVMe KV SSD Backend Design Document

## Overview

This document presents the complete design for **NVMe Key-Value (KV) SSD backend** in Mooncake Store. Unlike traditional LBA-based storage backends (Bucket, FilePerKey, OffsetAllocator), this backend leverages the **native NVMe KV command set** (NVMe TP 4077) for direct key-value operations on compatible SSDs.

### Key Advantages

| Aspect | LBA-based (Current) | NVMe KV Native |
|--------|---------------------|----------------|
| **I/O Path** | App → FileSystem → Block Layer → NVMe | App → NVMe KV Commands |
| **Lookup** | File system metadata traversal | O(1) hardware hash lookup |
| **Space Management** | File system allocator | Device-managed |
| **Write Amplification** | File system journaling + data | Direct KV write |

---

## Architecture

```
┌────────────────────────────────────────────────────────────────────┐
│                          FileStorage                               │
│                    (Top-level coordinator)                         │
└──────────────────────────────┬─────────────────────────────────────┘
                               │
                               ▼
┌────────────────────────────────────────────────────────────────────┐
│                    StorageBackendInterface                          │
│                                                                     │
│   ┌────────────┬────────────┬──────────────┬──────────────────┐   │
│   │  Bucket    │ FilePerKey │  Offset      │  NVMe KV         │   │
│   │  Backend   │  Backend   │  Allocator   │  Backend (NEW)   │   │
│   │            │            │              │                   │   │
│   │ LBA-based  │ LBA-based  │  LBA-based   │  KV-native       │   │
│   └────────────┴────────────┴──────────────┴──────────────────┘   │
└──────────────────────────────┬─────────────────────────────────────┘
                               │
            ┌──────────────────┴──────────────────┐
            │                                     │
            ▼                                     ▼
┌───────────────────────┐          ┌──────────────────────────────┐
│    StorageFile        │          │      NvmeKvDevice            │
│  (UringFile/Posix)    │          │   Linux ioctl / SPDK         │
│                       │          │                              │
│  File system I/O      │          │  Direct NVMe KV commands     │
│  pread/pwrite         │          │  Store/Retrieve/Delete/Exist │
└───────────────────────┘          └──────────────────────────────┘
            │                                     │
            ▼                                     ▼
       File System                          NVMe KV SSD
     (ext4/xfs + NVMe)                  (Native KV command set)
```

---

## Key Definitions

### KeyID Mapping

NVMe KV standard limits key size to **16 bytes**. For user keys longer than 16 bytes, we use a hash-based mapping:

```
KeyID = XXH3_128bits(salt || user_key)
```

#### Hash Algorithm: XXHash3 128-bit

We chose **XXH3_128bits** for the following reasons:

| Property | Value | Benefit |
|----------|-------|---------|
| **Output Size** | 128 bits (16 bytes) | Perfect fit for NVMe KV KeyID |
| **Throughput** | ~30 GB/s | Negligible overhead for key generation |
| **Avalanche** | Full avalanche | 1-bit input change → ~50% output bits flip |
| **Distribution** | Uniform | Even distribution across KeyID space |

#### Collision Analysis

For N keys with a 128-bit hash:
- **Collision probability** ≈ N² / 2^129 (birthday paradox)

| Number of Keys | Collision Probability |
|----------------|----------------------|
| 1 million | ~10^-27 |
| 1 billion | ~10^-21 |
| 10 billion | ~10^-19 |
| 1 trillion | ~10^-15 |

Even at 1 trillion keys, collision is practically impossible.

#### Salt for Domain Separation

```cpp
std::array<uint8_t, 16> hash_salt_;  // Randomly generated per backend instance
```

The salt ensures:
1. **Different backends** generate different KeyIDs for the same user key
2. **Multi-tenant isolation** when sharing the same NVMe KV device
3. **Restart consistency** if salt is persisted

#### Implementation

```cpp
struct NvmeKvKeyId {
    static constexpr size_t SIZE = 16;
    uint8_t data[SIZE];
    
    // No-salt version (for simple use cases)
    explicit NvmeKvKeyId(const std::string& user_key);
    
    // Salted version (recommended for production)
    NvmeKvKeyId(const uint8_t* salt, size_t salt_len, const std::string& user_key);
};

// Backend usage
NvmeKvKeyId NvmeKvStorageBackend::GenerateKeyId(const std::string& user_key) const {
    return NvmeKvKeyId(hash_salt_.data(), hash_salt_.size(), user_key);
}
```

#### Hash Computation Details

```cpp
// Fast path: 8-byte salt uses XXH3_128bits_withSeed
if (salt_len == 8) {
    uint64_t seed;
    memcpy(&seed, salt, sizeof(seed));
    XXH128_hash_t hash = XXH3_128bits_withSeed(user_key.data(), user_key.size(), seed);
    // Store hash.low64 and hash.high64 into 16-byte KeyID
}

// General path: streaming API for variable-length salt
else {
    XXH3_state_t* state = XXH3_createState();
    XXH3_128bits_reset(state);
    XXH3_128bits_update(state, salt, salt_len);
    XXH3_128bits_update(state, user_key.data(), user_key.size());
    XXH128_hash_t hash = XXH3_128bits_digest(state);
    XXH3_freeState(state);
}
```

### Value Payload Structure

To enable **collision detection** and proper key verification on retrieval, the original `user_key` is stored within the value payload:

```
┌─────────────┬──────────────┬─────────────────┬─────────────────┐
│ key_len(4B) │ value_len(4B)│ key (variable)  │ value (variable)│
└─────────────┴──────────────┴─────────────────┴─────────────────┘
     u32           u32           key_len bytes    value_len bytes
```

This format aligns with `OffsetAllocatorStorageBackend` for consistency:

```cpp
struct RecordHeader {
    uint32_t key_len;
    uint32_t value_len;
    static constexpr size_t SIZE = 8;  // 8 bytes
};
```

#### Collision Detection on Read

```cpp
tl::expected<void, ErrorCode> BatchLoad(...) {
    // 1. Generate KeyID from user_key
    NvmeKvKeyId key_id = GenerateKeyId(user_key);
    
    // 2. Retrieve from device
    device_->Retrieve(key_id, buffer, buffer_size, &actual_size);
    
    // 3. Parse payload
    std::string stored_key;
    ParseValuePayload(buffer, actual_size, &stored_key, ...);
    
    // 4. Verify key matches (collision detection)
    if (stored_key != user_key) {
        LOG(ERROR) << "Hash collision detected: requested=" << user_key
                   << ", stored=" << stored_key;
        return tl::make_unexpected(ErrorCode::INTERNAL_ERROR);
    }
    
    // 5. Return value
}
```

---

## Core Components

### NvmeKvDevice Abstraction

`NvmeKvDevice` provides a platform-independent interface for NVMe KV operations:

```cpp
class NvmeKvDevice {
public:
    virtual tl::expected<void, ErrorCode> Init() = 0;
    virtual void Close() = 0;
    
    // Core KV operations
    virtual tl::expected<void, ErrorCode> Store(
        const NvmeKvKeyId& key_id, const void* value, size_t value_size) = 0;
    
    virtual tl::expected<void, ErrorCode> Retrieve(
        const NvmeKvKeyId& key_id, void* buffer, size_t buffer_size,
        size_t* actual_size) = 0;
    
    virtual tl::expected<void, ErrorCode> Delete(const NvmeKvKeyId& key_id) = 0;
    
    virtual tl::expected<bool, ErrorCode> Exist(const NvmeKvKeyId& key_id) = 0;
    
    // Batch operations
    virtual tl::expected<void, ErrorCode> BatchSubmit(
        std::vector<NvmeKvBatchOp>& ops) = 0;
    
    virtual int WaitCompletion(uint32_t timeout_ms = 0) = 0;
    
    // Capacity query
    virtual tl::expected<void, ErrorCode> GetCapacity(
        uint64_t* total_capacity, uint64_t* used_capacity) = 0;
};
```

**Implementation Options:**

| Implementation | Use Case |
|---------------|----------|
| `LinuxNvmeKvDevice` | Standard Linux NVMe ioctl passthrough |
| `SpdkNvmeKvDevice` | SPDK userspace driver (future) |
| `VendorNvmeKvDevice` | Vendor-specific SDK (Samsung, WD, etc.) |

### NvmeKvStorageBackend

`NvmeKvStorageBackend` implements `StorageBackendInterface` with NVMe KV native semantics:

```cpp
class NvmeKvStorageBackend : public StorageBackendInterface {
public:
    tl::expected<void, ErrorCode> Init() override;
    
    tl::expected<int64_t, ErrorCode> BatchOffload(
        const std::unordered_map<std::string, std::vector<Slice>>& batch_object,
        std::function<ErrorCode(...)> complete_handler,
        std::function<void(...)> eviction_handler) override;
    
    tl::expected<void, ErrorCode> BatchLoad(
        std::unordered_map<std::string, Slice>& batched_slices) override;
    
    tl::expected<bool, ErrorCode> IsExist(const std::string& key) override;
    tl::expected<bool, ErrorCode> IsEnableOffloading() override;
    tl::expected<void, ErrorCode> ScanMeta(...) override;
};
```

---

## Data Flow

### BatchOffload (Memory → NVMe KV SSD)

```
FileStorage::OffloadObjects
    │
    ▼
┌─────────────────────────────────────────────────────────────────┐
│ NvmeKvStorageBackend::BatchOffload                              │
│                                                                  │
│  For each (key, slices) in batch_object:                        │
│    1. Generate KeyID: KeyID = Hash128(salt || key)              │
│    2. Build payload: [key_len | value_len | key | value]        │
│    3. Call NvmeKvDevice::Store(KeyID, payload)                  │
│    4. Update host index: shards_[shard].map[key] = ObjectEntry  │
│    5. Update atomic counters: total_keys_++, total_size_ += ... │
│                                                                  │
│  On success: call complete_handler(keys, metadatas)             │
└─────────────────────────────────────────────────────────────────┘
    │
    ▼ NVMe KV Store Command
┌─────────────────────────────────────────────────────────────────┐
│  NVMe KV SSD                                                     │
│  KeyID (16B) → Value (payload)                                   │
└─────────────────────────────────────────────────────────────────┘
```

### BatchLoad (NVMe KV SSD → Memory)

```
FileStorage::BatchGet
    │
    ▼
┌─────────────────────────────────────────────────────────────────┐
│ NvmeKvStorageBackend::BatchLoad                                  │
│                                                                  │
│  For each (key, slice) in batched_slices:                       │
│    1. Lookup host index: entry = shards_[shard].map[key]        │
│    2. Call NvmeKvDevice::Retrieve(entry.key_id, buffer)         │
│    3. Parse payload: extract stored_key and value               │
│    4. Verify: stored_key == key (collision detection)           │
│    5. Copy value to slice.data                                  │
└─────────────────────────────────────────────────────────────────┘
    │
    ▼ NVMe KV Retrieve Command
┌─────────────────────────────────────────────────────────────────┐
│  NVMe KV SSD                                                     │
│  KeyID (16B) → Value (payload)                                   │
└─────────────────────────────────────────────────────────────────┘
```

---

## Host Index

### Purpose

Unlike file-based backends that can scan the filesystem, NVMe KV devices typically don't support efficient key enumeration. A **host-side index** maintains:

1. Fast key existence checks without device I/O
2. Efficient `ScanMeta` iteration
3. Metadata for eviction decisions (timestamps, sizes)

### Sharded Design

To reduce lock contention under high concurrency, the host index is sharded across 1024 maps:

```cpp
static constexpr size_t kNumShards = 1024;

struct MetadataShard {
    mutable SharedMutex mutex;
    std::unordered_map<std::string, ObjectEntry> map;
};

std::array<MetadataShard, kNumShards> shards_;

inline size_t ShardForKey(const std::string& key) const {
    return std::hash<std::string>{}(key) & (kNumShards - 1);
}
```

### ObjectEntry Structure

```cpp
struct ObjectEntry {
    NvmeKvKeyId key_id;        // Device-level key (16 bytes)
    uint32_t key_size;         // Original user key size
    uint32_t value_size;       // Value size (excluding header)
    uint64_t store_timestamp;  // Nanoseconds since epoch
};
```

### Persistence

The host index is persisted to the NVMe KV device using a reserved KeyID prefix:

```cpp
static constexpr char kHostIndexKeyPrefix[] = "__MOONCAKE_HOST_INDEX__";
```

On startup, `Init()` attempts to load the persisted index. On shutdown or checkpoint, the index is serialized and stored.

---

## Eviction Protocol

### Two-Phase Eviction (Consistent with BucketStorageBackend)

**Phase 1 - PrepareEviction (under exclusive lock):**

```cpp
PendingEviction PrepareEviction(int64_t required_count) {
    // 1. Check if eviction needed
    if (current_keys + required_count <= max_keys) return {};
    
    // 2. Calculate eviction count (at least 10% of max)
    int64_t evict_count = std::max(
        current_keys + required_count - max_keys,
        max_keys * 0.1);
    
    // 3. Select candidates (FIFO by store_timestamp)
    // 4. Remove from host index
    // 5. Return PendingEviction with keys and key_ids
}
```

**Between phases - Notify Master:**

```cpp
if (eviction_handler && !pending.keys.empty()) {
    eviction_handler(pending.keys);  // Calls BatchEvictDiskReplica
}
```

**Phase 2 - FinalizeEviction (no lock):**

```cpp
void FinalizeEviction(const PendingEviction& pending) {
    for (const auto& key_id : pending.key_ids) {
        device_->Delete(key_id);  // NVMe KV Delete command
    }
}
```

### Eviction Policy

| Policy | Selection Criteria |
|--------|-------------------|
| **FIFO** (default) | Oldest `store_timestamp` first |
| **LRU** (future) | Track `last_access_ns_` on reads |

---

## Persistence and Recovery

### Write-Ahead Logging (WAL) - Optional

When `wal_enabled = true`:

```cpp
tl::expected<void, ErrorCode> WriteWalEntry(
    const std::string& key,
    const NvmeKvKeyId& key_id,
    bool is_delete);

tl::expected<void, ErrorCode> RecoverFromWal();
tl::expected<void, ErrorCode> CheckpointWal();
```

WAL format:
```
┌────────────┬──────────────┬────────────┬─────────────────┐
│ magic (4B) │ entry_type   │ key_id(16B)│ key (variable)  │
└────────────┴──────────────┴────────────┴─────────────────┘
```

### Recovery Flow

```
Init()
  │
  ├─► LoadHostIndex()          # Try to load persisted index
  │       │
  │       ├─► Success: Index loaded
  │       └─► Failure: Start with empty index
  │
  └─► RecoverFromWal()         # Replay WAL entries (if enabled)
          │
          └─► Checkpoint and truncate WAL
```

---

## Configuration

### Environment Variables

| Variable | Default | Description |
|----------|---------|-------------|
| `MOONCAKE_OFFLOAD_STORAGE_BACKEND_DESCRIPTOR` | `bucket_storage_backend` | Set to `nvme_kv_storage_backend` |
| `MOONCAKE_NVME_KV_DEVICE_PATH` | `/dev/nvme0n1` | NVMe KV device path |
| `MOONCAKE_NVME_KV_MAX_ENTRIES` | `100000000` | Maximum number of KV entries |
| `MOONCAKE_NVME_KV_EVICTION_THRESHOLD` | `0.9` | Trigger eviction when usage exceeds |
| `MOONCAKE_NVME_KV_WAL_ENABLED` | `false` | Enable write-ahead logging |
| `MOONCAKE_NVME_KV_WAL_PATH` | `/tmp/mooncake_nvmekv_wal` | WAL directory |
| `MOONCAKE_NVME_KV_QUEUE_DEPTH` | `64` | I/O queue depth |
| `MOONCAKE_NVME_KV_ENABLE_HOST_INDEX` | `true` | Enable host-side index |

### NvmeKvBackendConfig

```cpp
struct NvmeKvBackendConfig {
    std::string device_path = "/dev/nvme0n1";
    uint64_t max_entries = 100000000;
    double eviction_threshold = 0.9;
    bool wal_enabled = false;
    std::string wal_path = "/tmp/mooncake_nvmekv_wal";
    static constexpr size_t kNumShards = 1024;
    bool enable_host_index = true;
    uint32_t queue_depth = 64;
    
    bool Validate() const;
    static NvmeKvBackendConfig FromEnvironment();
};
```

---

## Interface Semantic Consistency

| Feature | BucketStorageBackend | NvmeKvStorageBackend |
|---------|---------------------|----------------------|
| **Two-phase eviction** | ✅ PrepareEviction + FinalizeEviction | ✅ Identical |
| **Metadata sharding** | ❌ Single map | ✅ 1024 shards |
| **complete_handler** | ✅ Notify Master (LOCAL_DISK replica) | ✅ Identical |
| **eviction_handler** | ✅ Notify Master (remove replica) | ✅ Identical |
| **Atomic counters** | ❌ Locked updates | ✅ Lock-free |
| **Value format** | Custom bucket format | `[key_len\|value_len\|key\|value]` |
| **ScanMeta** | File system scan | Host index iteration |

---

## File Structure

```
mooncake-store/
├── include/
│   ├── nvme_kv_device.h           # NvmeKvDevice interface
│   ├── nvme_kv_storage_backend.h  # NvmeKvStorageBackend class
│   └── storage_backend.h          # Updated with kNvmeKv enum
├── src/
│   ├── nvme_kv_device.cpp         # LinuxNvmeKvDevice implementation
│   ├── nvme_kv_storage_backend.cpp # Backend implementation
│   ├── storage_backend.cpp        # Updated CreateStorageBackend
│   └── file_storage.cpp           # Updated FromEnvironment
└── CMakeLists.txt                 # Build configuration
```

---

## Usage Example

### Environment Configuration

```bash
# Select NVMe KV backend
export MOONCAKE_OFFLOAD_STORAGE_BACKEND_DESCRIPTOR="nvme_kv_storage_backend"

# Device configuration
export MOONCAKE_NVME_KV_DEVICE_PATH="/dev/nvme0n1"
export MOONCAKE_NVME_KV_MAX_ENTRIES="100000000"
export MOONCAKE_NVME_KV_EVICTION_THRESHOLD="0.9"

# Enable offload
export MOONCAKE_ENABLE_OFFLOAD="true"
```

### Programmatic Usage

```cpp
#include "nvme_kv_storage_backend.h"

// Create configuration
NvmeKvBackendConfig nvme_config;
nvme_config.device_path = "/dev/nvme0n1";
nvme_config.max_entries = 100000000;
nvme_config.eviction_threshold = 0.9;

FileStorageConfig fs_config;
fs_config.storage_backend_type = StorageBackendType::kNvmeKv;

// Create backend
auto backend = std::make_shared<NvmeKvStorageBackend>(fs_config, nvme_config);
auto init_result = backend->Init();
if (!init_result) {
    LOG(ERROR) << "Failed to initialize NVMe KV backend";
    return;
}

// Use via standard interface
std::unordered_map<std::string, std::vector<Slice>> batch;
// ... populate batch ...

auto offload_result = backend->BatchOffload(batch, complete_handler, eviction_handler);
```

---

## Future Work

1. **SPDK Integration**: Userspace NVMe driver for lower latency
2. **Async I/O**: io_uring-based batch submission for NVMe KV commands
3. **LRU Eviction**: Track read access for smarter eviction decisions
4. **Vendor Optimizations**: Samsung KV SSD SDK, Western Digital ZNS+KV
5. **Compression**: Optional value compression before storage
6. **Tiered Storage**: Integration with CXL memory tier

---

## References

- [NVMe TP 4077 - Key Value Command Set](https://nvmexpress.org/specifications/)
- [Samsung Key Value SSD](https://semiconductor.samsung.com/ssd/key-value-ssd/)
- Mooncake Store SSD Offload Design: `docs/source/design/ssd-offload.md`
