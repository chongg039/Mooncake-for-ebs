#pragma once

#include "nvme_kv_device.h"

#include <mutex>
#include <unordered_map>
#include <unordered_set>
#include <vector>
#include <atomic>
#include <thread>
#include <chrono>

#include <glog/logging.h>

namespace mooncake {

/**
 * @brief Mock implementation of NvmeKvDevice for testing
 * 
 * This mock simulates an NVMe KV SSD using an in-memory hash map.
 * It allows testing of NvmeKvStorageBackend without requiring actual
 * NVMe KV hardware.
 * 
 * Features:
 * - Thread-safe in-memory KV store
 * - Configurable capacity limits
 * - Simulated latency (optional)
 * - Error injection support
 */
class MockNvmeKvDevice : public NvmeKvDevice {
   public:
    struct MockConfig {
        uint64_t max_entries;
        uint64_t max_total_bytes;
        uint32_t max_value_size;
        bool simulate_latency;
        uint32_t latency_us;
        
        MockConfig() 
            : max_entries(1000000)
            , max_total_bytes(1ULL << 30)
            , max_value_size(2 * 1024 * 1024)
            , simulate_latency(false)
            , latency_us(100) {}
    };

    explicit MockNvmeKvDevice(const MockConfig& config = MockConfig())
        : config_(config), initialized_(false), 
          total_entries_(0), total_bytes_(0) {}

    ~MockNvmeKvDevice() override { Close(); }

    tl::expected<void, ErrorCode> Init() override {
        if (initialized_.load(std::memory_order_acquire)) {
            return {};
        }

        LOG(INFO) << "MockNvmeKvDevice initialized: max_entries=" 
                  << config_.max_entries
                  << ", max_total_bytes=" << config_.max_total_bytes;

        initialized_.store(true, std::memory_order_release);
        return {};
    }

    void Close() override {
        if (initialized_.load(std::memory_order_acquire)) {
            std::lock_guard<std::mutex> lock(mutex_);
            store_.clear();
            total_entries_.store(0, std::memory_order_relaxed);
            total_bytes_.store(0, std::memory_order_relaxed);
            initialized_.store(false, std::memory_order_release);
            LOG(INFO) << "MockNvmeKvDevice closed";
        }
    }

    tl::expected<void, ErrorCode> Store(
        const NvmeKvKeyId& key_id,
        const void* value,
        size_t value_size) override {
        
        if (!initialized_.load(std::memory_order_acquire)) {
            return tl::make_unexpected(ErrorCode::INTERNAL_ERROR);
        }

        if (value_size > config_.max_value_size) {
            return tl::make_unexpected(ErrorCode::INVALID_PARAMS);
        }

        // Check error injection
        if (ShouldInjectError(key_id)) {
            return tl::make_unexpected(ErrorCode::INTERNAL_ERROR);
        }

        SimulateLatency();

        std::string key_str(reinterpret_cast<const char*>(key_id.data), 
                           NvmeKvKeyId::SIZE);
        std::string value_str(static_cast<const char*>(value), value_size);

        std::lock_guard<std::mutex> lock(mutex_);

        // Check capacity
        auto it = store_.find(key_str);
        bool is_update = (it != store_.end());
        
        if (!is_update) {
            if (total_entries_.load() >= config_.max_entries) {
                return tl::make_unexpected(ErrorCode::KEYS_ULTRA_LIMIT);
            }
            if (total_bytes_.load() + value_size > config_.max_total_bytes) {
                return tl::make_unexpected(ErrorCode::KEYS_ULTRA_LIMIT);
            }
        }

        // Store or update
        if (is_update) {
            // Update: adjust bytes
            int64_t diff = static_cast<int64_t>(value_size) - 
                          static_cast<int64_t>(it->second.size());
            total_bytes_.fetch_add(diff, std::memory_order_relaxed);
            it->second = std::move(value_str);
        } else {
            // New entry
            store_.emplace(key_str, std::move(value_str));
            total_entries_.fetch_add(1, std::memory_order_relaxed);
            total_bytes_.fetch_add(value_size, std::memory_order_relaxed);
        }

        return {};
    }

    tl::expected<void, ErrorCode> Retrieve(
        const NvmeKvKeyId& key_id,
        void* buffer,
        size_t buffer_size,
        size_t* actual_size) override {
        
        if (!initialized_.load(std::memory_order_acquire)) {
            return tl::make_unexpected(ErrorCode::INTERNAL_ERROR);
        }

        if (ShouldInjectError(key_id)) {
            return tl::make_unexpected(ErrorCode::INTERNAL_ERROR);
        }

        SimulateLatency();

        std::string key_str(reinterpret_cast<const char*>(key_id.data), 
                           NvmeKvKeyId::SIZE);

        std::lock_guard<std::mutex> lock(mutex_);

        auto it = store_.find(key_str);
        if (it == store_.end()) {
            return tl::make_unexpected(ErrorCode::OBJECT_NOT_FOUND);
        }

        const std::string& value = it->second;
        size_t copy_size = std::min(buffer_size, value.size());
        
        if (buffer && copy_size > 0) {
            memcpy(buffer, value.data(), copy_size);
        }

        if (actual_size) {
            *actual_size = value.size();
        }

        return {};
    }

    tl::expected<void, ErrorCode> Delete(const NvmeKvKeyId& key_id) override {
        if (!initialized_.load(std::memory_order_acquire)) {
            return tl::make_unexpected(ErrorCode::INTERNAL_ERROR);
        }

        SimulateLatency();

        std::string key_str(reinterpret_cast<const char*>(key_id.data), 
                           NvmeKvKeyId::SIZE);

        std::lock_guard<std::mutex> lock(mutex_);

        auto it = store_.find(key_str);
        if (it == store_.end()) {
            return tl::make_unexpected(ErrorCode::OBJECT_NOT_FOUND);
        }

        size_t value_size = it->second.size();
        store_.erase(it);
        total_entries_.fetch_sub(1, std::memory_order_relaxed);
        total_bytes_.fetch_sub(value_size, std::memory_order_relaxed);

        return {};
    }

    tl::expected<bool, ErrorCode> Exist(const NvmeKvKeyId& key_id) override {
        if (!initialized_.load(std::memory_order_acquire)) {
            return tl::make_unexpected(ErrorCode::INTERNAL_ERROR);
        }

        std::string key_str(reinterpret_cast<const char*>(key_id.data), 
                           NvmeKvKeyId::SIZE);

        std::lock_guard<std::mutex> lock(mutex_);
        return store_.find(key_str) != store_.end();
    }

    tl::expected<void, ErrorCode> BatchSubmit(
        std::vector<NvmeKvBatchOp>& ops) override {
        
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
                    cmd_result = Retrieve(op.key_id, op.buffer, 
                                         op.buffer_size, &actual_size);
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
                    result.error_code = cmd_result ? ErrorCode::OK 
                                                   : cmd_result.error();
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

    int WaitCompletion(uint32_t /* timeout_ms */) override {
        // Synchronous mock - all operations complete immediately
        return 0;
    }

    tl::expected<void, ErrorCode> GetCapacity(
        uint64_t* total_capacity,
        uint64_t* used_capacity) override {
        
        if (!initialized_.load(std::memory_order_acquire)) {
            return tl::make_unexpected(ErrorCode::INTERNAL_ERROR);
        }

        if (total_capacity) {
            *total_capacity = config_.max_total_bytes;
        }
        if (used_capacity) {
            *used_capacity = total_bytes_.load(std::memory_order_relaxed);
        }
        return {};
    }

    // ========== Test Helper Methods ==========

    /**
     * @brief Get the number of stored entries
     */
    size_t GetEntryCount() const {
        return total_entries_.load(std::memory_order_relaxed);
    }

    /**
     * @brief Get total bytes stored
     */
    size_t GetTotalBytes() const {
        return total_bytes_.load(std::memory_order_relaxed);
    }

    /**
     * @brief Clear all stored data
     */
    void Clear() {
        std::lock_guard<std::mutex> lock(mutex_);
        store_.clear();
        total_entries_.store(0, std::memory_order_relaxed);
        total_bytes_.store(0, std::memory_order_relaxed);
    }

    /**
     * @brief Set error injection for specific KeyID
     */
    void InjectError(const NvmeKvKeyId& key_id) {
        std::string key_str(reinterpret_cast<const char*>(key_id.data), 
                           NvmeKvKeyId::SIZE);
        std::lock_guard<std::mutex> lock(error_mutex_);
        error_keys_.insert(key_str);
    }

    /**
     * @brief Clear all error injections
     */
    void ClearErrorInjections() {
        std::lock_guard<std::mutex> lock(error_mutex_);
        error_keys_.clear();
    }

   private:
    bool ShouldInjectError(const NvmeKvKeyId& key_id) {
        std::string key_str(reinterpret_cast<const char*>(key_id.data), 
                           NvmeKvKeyId::SIZE);
        std::lock_guard<std::mutex> lock(error_mutex_);
        return error_keys_.find(key_str) != error_keys_.end();
    }

    void SimulateLatency() {
        if (config_.simulate_latency && config_.latency_us > 0) {
            std::this_thread::sleep_for(
                std::chrono::microseconds(config_.latency_us));
        }
    }

   private:
    MockConfig config_;
    std::atomic<bool> initialized_;
    
    // KV store
    mutable std::mutex mutex_;
    std::unordered_map<std::string, std::string> store_;
    std::atomic<uint64_t> total_entries_;
    std::atomic<uint64_t> total_bytes_;

    // Error injection
    mutable std::mutex error_mutex_;
    std::unordered_set<std::string> error_keys_;
};

}  // namespace mooncake
