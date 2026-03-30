#include "nvme_kv_device.h"

#include <fcntl.h>
#include <unistd.h>
#include <sys/ioctl.h>
#include <linux/nvme_ioctl.h>
#include <errno.h>
#include <cstring>
#include <condition_variable>

#include <xxhash.h>
#include <glog/logging.h>

#include "utils.h"

namespace mooncake {

// ============================================================================
// NvmeKvKeyId Implementation
// ============================================================================

/**
 * @brief Hash Strategy for NVMe KV KeyID Generation
 * 
 * We use XXHash3 128-bit (XXH3_128bits) for the following reasons:
 * 
 * 1. **High Quality**: XXH3 has excellent avalanche properties and distribution
 * 2. **Performance**: ~30 GB/s on modern CPUs, faster than cryptographic hashes
 * 3. **128-bit Output**: Perfect fit for NVMe KV 16-byte KeyID
 * 4. **Collision Resistance**: For N keys, collision probability is ~N^2/2^128
 *    - With 1 billion keys: ~10^-20 collision probability
 *    - With 10 billion keys: ~10^-18 collision probability
 * 
 * The hash is computed as: XXH3_128bits(salt || user_key)
 * - Salt provides domain separation between different backend instances
 * - User key is the application-level key (variable length)
 * 
 * Collision Detection:
 * - The original user_key is stored in the value payload
 * - On retrieval, we verify stored_key == requested_key
 * - This catches the rare hash collision case
 */

NvmeKvKeyId::NvmeKvKeyId(const std::string& user_key) {
    // No salt version - uses XXH3_128bits directly
    XXH128_hash_t hash = XXH3_128bits(user_key.data(), user_key.size());
    
    // XXH128_hash_t contains low64 and high64
    // Store in little-endian format
    memcpy(data, &hash.low64, sizeof(uint64_t));
    memcpy(data + sizeof(uint64_t), &hash.high64, sizeof(uint64_t));
}

NvmeKvKeyId::NvmeKvKeyId(const uint8_t* salt, size_t salt_len, 
                         const std::string& user_key) {
    // Salted version - concatenate salt and user_key, then hash
    // 
    // For efficiency, we use XXH3_128bits_withSeed when salt is 8 bytes,
    // otherwise fall back to concatenation approach
    
    if (salt_len == sizeof(uint64_t)) {
        // Use salt as seed (fast path)
        uint64_t seed;
        memcpy(&seed, salt, sizeof(seed));
        XXH128_hash_t hash = XXH3_128bits_withSeed(
            user_key.data(), user_key.size(), seed);
        memcpy(data, &hash.low64, sizeof(uint64_t));
        memcpy(data + sizeof(uint64_t), &hash.high64, sizeof(uint64_t));
    } else {
        // General case: hash(salt || user_key) using streaming API
        XXH3_state_t* state = XXH3_createState();
        if (state == nullptr) {
            LOG(ERROR) << "Failed to create XXH3 state";
            memset(data, 0, SIZE);
            return;
        }
        
        XXH3_128bits_reset(state);
        XXH3_128bits_update(state, salt, salt_len);
        XXH3_128bits_update(state, user_key.data(), user_key.size());
        XXH128_hash_t hash = XXH3_128bits_digest(state);
        XXH3_freeState(state);
        
        memcpy(data, &hash.low64, sizeof(uint64_t));
        memcpy(data + sizeof(uint64_t), &hash.high64, sizeof(uint64_t));
    }
}

NvmeKvKeyId NvmeKvKeyId::FromRawBytes(const uint8_t* bytes) {
    NvmeKvKeyId key_id;
    memcpy(key_id.data, bytes, SIZE);
    return key_id;
}

size_t NvmeKvKeyId::hash() const {
    // Use first 8 bytes as hash for std::unordered_map
    size_t h;
    memcpy(&h, data, sizeof(h));
    return h;
}

std::string NvmeKvKeyId::to_hex_string() const {
    static const char hex_chars[] = "0123456789abcdef";
    std::string result;
    result.reserve(SIZE * 2);
    for (size_t i = 0; i < SIZE; ++i) {
        result.push_back(hex_chars[(data[i] >> 4) & 0xF]);
        result.push_back(hex_chars[data[i] & 0xF]);
    }
    return result;
}

// ============================================================================
// NvmeKvDeviceConfig Implementation
// ============================================================================

bool NvmeKvDeviceConfig::Validate() const {
    if (device_path.empty()) {
        LOG(ERROR) << "NvmeKvDeviceConfig: device_path is empty";
        return false;
    }
    if (queue_depth == 0 || queue_depth > 1024) {
        LOG(ERROR) << "NvmeKvDeviceConfig: queue_depth must be in [1, 1024]";
        return false;
    }
    if (max_value_size == 0) {
        LOG(ERROR) << "NvmeKvDeviceConfig: max_value_size must be > 0";
        return false;
    }
    return true;
}

NvmeKvDeviceConfig NvmeKvDeviceConfig::FromEnvironment() {
    NvmeKvDeviceConfig config;

    config.device_path = GetEnvStringOr(
        "MOONCAKE_NVME_KV_DEVICE_PATH", config.device_path);
    config.queue_depth = GetEnvOr<uint32_t>(
        "MOONCAKE_NVME_KV_QUEUE_DEPTH", config.queue_depth);
    config.max_key_size = GetEnvOr<uint32_t>(
        "MOONCAKE_NVME_KV_MAX_KEY_SIZE", config.max_key_size);
    config.max_value_size = GetEnvOr<uint32_t>(
        "MOONCAKE_NVME_KV_MAX_VALUE_SIZE", config.max_value_size);
    config.enable_async = GetEnvOr<bool>(
        "MOONCAKE_NVME_KV_ENABLE_ASYNC", config.enable_async);
    config.io_threads = GetEnvOr<uint32_t>(
        "MOONCAKE_NVME_KV_IO_THREADS", config.io_threads);

    return config;
}

// ============================================================================
// NvmeKvDevice Factory
// ============================================================================

std::unique_ptr<NvmeKvDevice> NvmeKvDevice::Create(
    const NvmeKvDeviceConfig& config) {
    return std::make_unique<LinuxNvmeKvDevice>(config);
}

// ============================================================================
// LinuxNvmeKvDevice Implementation
// ============================================================================

// NVMe KV command specific opcodes (NVMe TP 4077)
// These may vary by vendor; adjust as needed
namespace {

// NVMe Admin command for KV (passthrough)
constexpr uint8_t NVME_KV_STORE_OPCODE = 0x81;
constexpr uint8_t NVME_KV_RETRIEVE_OPCODE = 0x82;
constexpr uint8_t NVME_KV_DELETE_OPCODE = 0x90;
constexpr uint8_t NVME_KV_EXIST_OPCODE = 0x94;

// NVMe KV status codes
constexpr uint16_t NVME_SC_SUCCESS = 0x0000;
constexpr uint16_t NVME_SC_KEY_NOT_EXIST = 0x0310;
constexpr uint16_t NVME_SC_KEY_EXISTS = 0x0311;

struct NvmeKvCommand {
    uint8_t opcode;
    uint8_t flags;
    uint16_t command_id;
    uint32_t nsid;
    uint64_t rsvd2;
    uint64_t metadata;
    uint64_t prp1;
    uint64_t prp2;
    uint8_t key[16];      // 16-byte key
    uint32_t value_size;
    uint32_t key_size;
    uint32_t option;
    uint32_t rsvd3[3];
};

}  // namespace

struct LinuxNvmeKvDevice::AsyncContext {
    // Placeholder for async I/O context (io_uring or AIO)
    // Can be extended with proper async implementation
    std::atomic<int> pending_ops{0};
    std::mutex completion_mutex;
    std::condition_variable completion_cv;
};

LinuxNvmeKvDevice::LinuxNvmeKvDevice(const NvmeKvDeviceConfig& config)
    : config_(config), async_ctx_(std::make_unique<AsyncContext>()) {}

LinuxNvmeKvDevice::~LinuxNvmeKvDevice() {
    Close();
}

tl::expected<void, ErrorCode> LinuxNvmeKvDevice::Init() {
    if (initialized_.load(std::memory_order_acquire)) {
        return {};
    }

    if (!config_.Validate()) {
        return tl::make_unexpected(ErrorCode::INVALID_PARAMS);
    }

    // Open NVMe device
    fd_ = open(config_.device_path.c_str(), O_RDWR);
    if (fd_ < 0) {
        LOG(ERROR) << "Failed to open NVMe KV device: " << config_.device_path
                   << ", error: " << strerror(errno);
        return tl::make_unexpected(ErrorCode::FILE_OPEN_FAIL);
    }

    // Verify device supports KV command set
    // This would typically involve checking NVMe Identify Controller data
    // For now, we assume the device supports KV commands

    LOG(INFO) << "NVMe KV device initialized: " << config_.device_path
              << ", queue_depth=" << config_.queue_depth;

    initialized_.store(true, std::memory_order_release);
    return {};
}

void LinuxNvmeKvDevice::Close() {
    if (fd_ >= 0) {
        close(fd_);
        fd_ = -1;
    }
    initialized_.store(false, std::memory_order_release);
}

tl::expected<void, ErrorCode> LinuxNvmeKvDevice::SendKvCommand(
    NvmeKvOpcode opcode,
    const NvmeKvKeyId& key_id,
    void* buffer,
    size_t buffer_size,
    size_t* bytes_transferred) {
    
    if (!initialized_.load(std::memory_order_acquire)) {
        return tl::make_unexpected(ErrorCode::INTERNAL_ERROR);
    }

    // Build NVMe passthrough command
    struct nvme_passthru_cmd cmd;
    memset(&cmd, 0, sizeof(cmd));

    cmd.opcode = static_cast<uint8_t>(opcode);
    cmd.nsid = 1;  // Namespace ID, typically 1 for single-namespace devices
    cmd.addr = reinterpret_cast<uint64_t>(buffer);
    cmd.data_len = buffer_size;

    // Copy key to command
    memcpy(&cmd.cdw10, key_id.data, 8);
    memcpy(&cmd.cdw12, key_id.data + 8, 8);

    // Set value size in CDW14-15 for STORE/RETRIEVE
    cmd.cdw14 = buffer_size & 0xFFFFFFFF;
    cmd.cdw15 = (buffer_size >> 32) & 0xFFFFFFFF;

    // Execute command via ioctl
    int ret = ioctl(fd_, NVME_IOCTL_IO_CMD, &cmd);
    if (ret < 0) {
        LOG(ERROR) << "NVMe KV command failed: opcode=" 
                   << static_cast<int>(opcode)
                   << ", error=" << strerror(errno);
        return tl::make_unexpected(ErrorCode::INTERNAL_ERROR);
    }

    // Check command status
    uint16_t status = (cmd.result >> 16) & 0xFFFF;
    if (status != NVME_SC_SUCCESS) {
        if (status == NVME_SC_KEY_NOT_EXIST) {
            return tl::make_unexpected(ErrorCode::OBJECT_NOT_FOUND);
        }
        LOG(ERROR) << "NVMe KV command returned error status: 0x"
                   << std::hex << status;
        return tl::make_unexpected(ErrorCode::INTERNAL_ERROR);
    }

    if (bytes_transferred) {
        *bytes_transferred = cmd.result & 0xFFFF;  // Actual bytes transferred
    }

    return {};
}

tl::expected<void, ErrorCode> LinuxNvmeKvDevice::Store(
    const NvmeKvKeyId& key_id,
    const void* value,
    size_t value_size) {
    
    if (value_size > config_.max_value_size) {
        LOG(ERROR) << "Value size " << value_size << " exceeds max "
                   << config_.max_value_size;
        return tl::make_unexpected(ErrorCode::INVALID_PARAMS);
    }

    return SendKvCommand(
        NvmeKvOpcode::STORE,
        key_id,
        const_cast<void*>(value),
        value_size,
        nullptr);
}

tl::expected<void, ErrorCode> LinuxNvmeKvDevice::Retrieve(
    const NvmeKvKeyId& key_id,
    void* buffer,
    size_t buffer_size,
    size_t* actual_size) {
    
    size_t transferred = 0;
    auto result = SendKvCommand(
        NvmeKvOpcode::RETRIEVE,
        key_id,
        buffer,
        buffer_size,
        &transferred);
    
    if (result && actual_size) {
        *actual_size = transferred;
    }
    return result;
}

tl::expected<void, ErrorCode> LinuxNvmeKvDevice::Delete(
    const NvmeKvKeyId& key_id) {
    return SendKvCommand(
        NvmeKvOpcode::DELETE,
        key_id,
        nullptr,
        0,
        nullptr);
}

tl::expected<bool, ErrorCode> LinuxNvmeKvDevice::Exist(
    const NvmeKvKeyId& key_id) {
    auto result = SendKvCommand(
        NvmeKvOpcode::EXIST,
        key_id,
        nullptr,
        0,
        nullptr);
    
    if (result) {
        return true;
    }
    if (result.error() == ErrorCode::OBJECT_NOT_FOUND) {
        return false;
    }
    return tl::make_unexpected(result.error());
}

tl::expected<void, ErrorCode> LinuxNvmeKvDevice::BatchSubmit(
    std::vector<NvmeKvBatchOp>& ops) {
    
    // Simple synchronous implementation
    // For production, use io_uring or vendor-specific async API
    for (auto& op : ops) {
        NvmeKvResult result;
        result.bytes_transferred = 0;

        tl::expected<void, ErrorCode> cmd_result;
        switch (op.opcode) {
            case NvmeKvOpcode::STORE:
                cmd_result = Store(op.key_id, op.buffer, op.buffer_size);
                if (cmd_result) {
                    result.error_code = ErrorCode::OK;
                    result.bytes_transferred = op.buffer_size;
                } else {
                    result.error_code = cmd_result.error();
                }
                break;

            case NvmeKvOpcode::RETRIEVE: {
                size_t actual_size = 0;
                cmd_result = Retrieve(op.key_id, op.buffer, op.buffer_size, &actual_size);
                if (cmd_result) {
                    result.error_code = ErrorCode::OK;
                    result.bytes_transferred = actual_size;
                } else {
                    result.error_code = cmd_result.error();
                }
                break;
            }

            case NvmeKvOpcode::DELETE:
                cmd_result = Delete(op.key_id);
                result.error_code = cmd_result ? ErrorCode::OK : cmd_result.error();
                break;

            default:
                result.error_code = ErrorCode::INVALID_PARAMS;
                break;
        }

        if (op.callback) {
            op.callback(result);
        }
    }

    return {};
}

int LinuxNvmeKvDevice::WaitCompletion(uint32_t timeout_ms) {
    // For synchronous implementation, all ops complete immediately
    return 0;
}

tl::expected<void, ErrorCode> LinuxNvmeKvDevice::GetCapacity(
    uint64_t* total_capacity,
    uint64_t* used_capacity) {
    
    // TODO: Implement via NVMe Identify Namespace or vendor-specific command
    // For now, return placeholder values
    if (total_capacity) {
        *total_capacity = 1ULL << 40;  // 1 TB placeholder
    }
    if (used_capacity) {
        *used_capacity = 0;  // Would need device query
    }
    return {};
}

}  // namespace mooncake
