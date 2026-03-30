#include "nvme_kv_storage_backend.h"
#include "mock_nvme_kv_device.h"

#include <glog/logging.h>
#include <gtest/gtest.h>

#include <filesystem>
#include <memory>
#include <thread>
#include <vector>

namespace fs = std::filesystem;

namespace mooncake {
namespace test {

/**
 * @brief Test fixture for NvmeKvStorageBackend
 * 
 * Uses MockNvmeKvDevice to simulate NVMe KV SSD behavior without
 * requiring actual hardware.
 */
class NvmeKvStorageBackendTest : public ::testing::Test {
   protected:
    void SetUp() override {
        // Create test directory
        test_dir_ = fs::current_path() / "nvme_kv_test_data";
        if (fs::exists(test_dir_)) {
            fs::remove_all(test_dir_);
        }
        fs::create_directories(test_dir_);

        // Create configurations
        file_storage_config_.storage_filepath = test_dir_.string();
        file_storage_config_.total_size_limit = 1ULL << 30;  // 1GB

        nvme_kv_config_.device_path = "/dev/mock_nvme";  // Mock path
        nvme_kv_config_.max_entries = 100000;
        nvme_kv_config_.eviction_threshold = 0.9;
        nvme_kv_config_.enable_host_index = true;
    }

    void TearDown() override {
        backend_.reset();
        if (fs::exists(test_dir_)) {
            fs::remove_all(test_dir_);
        }
    }

    /**
     * @brief Create backend with mock device
     */
    void CreateBackend(const MockNvmeKvDevice::MockConfig& mock_config = {}) {
        auto mock_device = std::make_unique<MockNvmeKvDevice>(mock_config);
        mock_device_ = mock_device.get();  // Keep raw pointer for assertions

        backend_ = std::make_unique<NvmeKvStorageBackend>(
            file_storage_config_,
            nvme_kv_config_,
            std::move(mock_device));
    }

    /**
     * @brief Helper to create test data
     */
    std::string CreateTestData(size_t size, char fill = 'x') {
        return std::string(size, fill);
    }

    /**
     * @brief Helper to batch offload a single key-value pair
     */
    tl::expected<int64_t, ErrorCode> OffloadSingle(
        const std::string& key, 
        const std::string& value) {
        
        std::unordered_map<std::string, std::vector<Slice>> batch;
        batch[key] = {Slice{const_cast<char*>(value.data()), value.size()}};

        return backend_->BatchOffload(
            batch,
            [](const std::vector<std::string>&,
               std::vector<StorageObjectMetadata>&) {
                return ErrorCode::OK;
            });
    }

    /**
     * @brief Helper to batch load a single key
     */
    tl::expected<std::string, ErrorCode> LoadSingle(const std::string& key) {
        std::string buffer(64 * 1024, '\0');  // 64KB buffer
        std::unordered_map<std::string, Slice> batched_slices;
        batched_slices[key] = Slice{buffer.data(), buffer.size()};

        auto result = backend_->BatchLoad(batched_slices);
        if (!result) {
            return tl::make_unexpected(result.error());
        }

        // Trim to actual size
        buffer.resize(batched_slices[key].size);
        return buffer;
    }

   protected:
    fs::path test_dir_;
    FileStorageConfig file_storage_config_;
    NvmeKvBackendConfig nvme_kv_config_;
    std::unique_ptr<NvmeKvStorageBackend> backend_;
    MockNvmeKvDevice* mock_device_ = nullptr;  // Raw pointer to mock
};

// ============================================================================
// Basic Functionality Tests
// ============================================================================

TEST_F(NvmeKvStorageBackendTest, InitSucceeds) {
    CreateBackend();
    auto result = backend_->Init();
    ASSERT_TRUE(result.has_value()) << "Init should succeed";
}

TEST_F(NvmeKvStorageBackendTest, InitIdempotent) {
    CreateBackend();
    
    auto result1 = backend_->Init();
    ASSERT_TRUE(result1.has_value());
    
    auto result2 = backend_->Init();
    ASSERT_TRUE(result2.has_value()) << "Second Init should also succeed";
}

TEST_F(NvmeKvStorageBackendTest, OffloadAndLoadSingleKey) {
    CreateBackend();
    ASSERT_TRUE(backend_->Init().has_value());

    std::string key = "test_key_1";
    std::string value = "test_value_12345";

    // Offload
    auto offload_result = OffloadSingle(key, value);
    ASSERT_TRUE(offload_result.has_value());
    EXPECT_EQ(offload_result.value(), 1);

    // Load and verify
    auto load_result = LoadSingle(key);
    ASSERT_TRUE(load_result.has_value());
    EXPECT_EQ(load_result.value(), value);
}

TEST_F(NvmeKvStorageBackendTest, OffloadAndLoadLargeValue) {
    CreateBackend();
    ASSERT_TRUE(backend_->Init().has_value());

    std::string key = "large_key";
    std::string value = CreateTestData(1024 * 1024);  // 1MB

    // Offload
    auto offload_result = OffloadSingle(key, value);
    ASSERT_TRUE(offload_result.has_value());

    // Load and verify
    std::string buffer(2 * 1024 * 1024, '\0');  // 2MB buffer
    std::unordered_map<std::string, Slice> batched_slices;
    batched_slices[key] = Slice{buffer.data(), buffer.size()};

    auto load_result = backend_->BatchLoad(batched_slices);
    ASSERT_TRUE(load_result.has_value());
    
    buffer.resize(batched_slices[key].size);
    EXPECT_EQ(buffer, value);
}

TEST_F(NvmeKvStorageBackendTest, IsExistReturnsTrueForExistingKey) {
    CreateBackend();
    ASSERT_TRUE(backend_->Init().has_value());

    std::string key = "exists_key";
    std::string value = "some_value";

    ASSERT_TRUE(OffloadSingle(key, value).has_value());

    auto exists = backend_->IsExist(key);
    ASSERT_TRUE(exists.has_value());
    EXPECT_TRUE(exists.value());
}

TEST_F(NvmeKvStorageBackendTest, IsExistReturnsFalseForNonExistingKey) {
    CreateBackend();
    ASSERT_TRUE(backend_->Init().has_value());

    auto exists = backend_->IsExist("non_existing_key");
    ASSERT_TRUE(exists.has_value());
    EXPECT_FALSE(exists.value());
}

TEST_F(NvmeKvStorageBackendTest, LoadNonExistingKeyFails) {
    CreateBackend();
    ASSERT_TRUE(backend_->Init().has_value());

    auto result = LoadSingle("non_existing_key");
    EXPECT_FALSE(result.has_value());
    EXPECT_EQ(result.error(), ErrorCode::OBJECT_NOT_FOUND);
}

// ============================================================================
// Batch Operation Tests
// ============================================================================

TEST_F(NvmeKvStorageBackendTest, BatchOffloadMultipleKeys) {
    CreateBackend();
    ASSERT_TRUE(backend_->Init().has_value());

    std::unordered_map<std::string, std::vector<Slice>> batch;
    std::vector<std::string> values;

    const int num_keys = 100;
    for (int i = 0; i < num_keys; ++i) {
        std::string key = "batch_key_" + std::to_string(i);
        std::string value = "batch_value_" + std::to_string(i);
        values.push_back(value);
        batch[key] = {Slice{const_cast<char*>(values.back().data()), 
                           values.back().size()}};
    }

    auto result = backend_->BatchOffload(
        batch,
        [](const std::vector<std::string>&,
           std::vector<StorageObjectMetadata>&) {
            return ErrorCode::OK;
        });

    ASSERT_TRUE(result.has_value());
    EXPECT_EQ(result.value(), num_keys);

    // Verify mock device has correct entry count
    EXPECT_EQ(mock_device_->GetEntryCount(), static_cast<size_t>(num_keys));
}

TEST_F(NvmeKvStorageBackendTest, BatchLoadMultipleKeys) {
    CreateBackend();
    ASSERT_TRUE(backend_->Init().has_value());

    // Offload multiple keys
    std::unordered_map<std::string, std::string> test_data;
    for (int i = 0; i < 10; ++i) {
        std::string key = "load_key_" + std::to_string(i);
        std::string value = "load_value_" + std::to_string(i);
        test_data[key] = value;
        ASSERT_TRUE(OffloadSingle(key, value).has_value());
    }

    // Batch load all keys
    std::vector<std::string> buffers(10, std::string(1024, '\0'));
    std::unordered_map<std::string, Slice> batched_slices;
    int idx = 0;
    for (const auto& [key, _] : test_data) {
        batched_slices[key] = Slice{buffers[idx].data(), buffers[idx].size()};
        idx++;
    }

    auto result = backend_->BatchLoad(batched_slices);
    ASSERT_TRUE(result.has_value());

    // Verify all values
    for (const auto& [key, expected_value] : test_data) {
        auto& slice = batched_slices[key];
        std::string actual_value(static_cast<char*>(slice.ptr), slice.size);
        EXPECT_EQ(actual_value, expected_value) << "Mismatch for key: " << key;
    }
}

// ============================================================================
// ScanMeta Tests
// ============================================================================

TEST_F(NvmeKvStorageBackendTest, ScanMetaReturnsAllKeys) {
    CreateBackend();
    ASSERT_TRUE(backend_->Init().has_value());

    // Offload some keys
    std::set<std::string> expected_keys;
    for (int i = 0; i < 50; ++i) {
        std::string key = "scan_key_" + std::to_string(i);
        std::string value = "scan_value_" + std::to_string(i);
        expected_keys.insert(key);
        ASSERT_TRUE(OffloadSingle(key, value).has_value());
    }

    // Scan and collect all keys
    std::set<std::string> scanned_keys;
    auto result = backend_->ScanMeta(
        [&](const std::vector<std::string>& keys,
            std::vector<StorageObjectMetadata>& metadatas) {
            for (const auto& key : keys) {
                scanned_keys.insert(key);
            }
            return ErrorCode::OK;
        });

    ASSERT_TRUE(result.has_value());
    EXPECT_EQ(scanned_keys, expected_keys);
}

// ============================================================================
// Update/Overwrite Tests
// ============================================================================

TEST_F(NvmeKvStorageBackendTest, OverwriteExistingKey) {
    CreateBackend();
    ASSERT_TRUE(backend_->Init().has_value());

    std::string key = "overwrite_key";
    std::string value1 = "original_value";
    std::string value2 = "updated_value_which_is_longer";

    // Initial write
    ASSERT_TRUE(OffloadSingle(key, value1).has_value());
    auto load1 = LoadSingle(key);
    ASSERT_TRUE(load1.has_value());
    EXPECT_EQ(load1.value(), value1);

    // Overwrite
    ASSERT_TRUE(OffloadSingle(key, value2).has_value());
    auto load2 = LoadSingle(key);
    ASSERT_TRUE(load2.has_value());
    EXPECT_EQ(load2.value(), value2);

    // Entry count should still be 1
    EXPECT_EQ(mock_device_->GetEntryCount(), 1u);
}

// ============================================================================
// Capacity and Offloading Check Tests
// ============================================================================

TEST_F(NvmeKvStorageBackendTest, IsEnableOffloadingInitiallyTrue) {
    CreateBackend();
    ASSERT_TRUE(backend_->Init().has_value());

    auto result = backend_->IsEnableOffloading();
    ASSERT_TRUE(result.has_value());
    EXPECT_TRUE(result.value());
}

TEST_F(NvmeKvStorageBackendTest, GetCapacityReturnsCorrectValues) {
    MockNvmeKvDevice::MockConfig mock_config;
    mock_config.max_total_bytes = 100 * 1024 * 1024;  // 100MB
    CreateBackend(mock_config);
    ASSERT_TRUE(backend_->Init().has_value());

    uint64_t total, used;
    auto result = backend_->GetCapacity(&total, &used);
    ASSERT_TRUE(result.has_value());
    EXPECT_EQ(total, 100 * 1024 * 1024);
    EXPECT_EQ(used, 0u);

    // Add some data
    std::string key = "capacity_key";
    std::string value = CreateTestData(1024);  // 1KB
    ASSERT_TRUE(OffloadSingle(key, value).has_value());

    result = backend_->GetCapacity(&total, &used);
    ASSERT_TRUE(result.has_value());
    EXPECT_GT(used, 0u);  // Should be > 0 (includes key in payload)
}

// ============================================================================
// Multi-Slice Value Tests
// ============================================================================

TEST_F(NvmeKvStorageBackendTest, OffloadMultiSliceValue) {
    CreateBackend();
    ASSERT_TRUE(backend_->Init().has_value());

    std::string key = "multi_slice_key";
    std::string part1 = "part1_data_";
    std::string part2 = "part2_data_";
    std::string part3 = "part3_data_";

    std::unordered_map<std::string, std::vector<Slice>> batch;
    batch[key] = {
        Slice{const_cast<char*>(part1.data()), part1.size()},
        Slice{const_cast<char*>(part2.data()), part2.size()},
        Slice{const_cast<char*>(part3.data()), part3.size()},
    };

    auto result = backend_->BatchOffload(
        batch,
        [](const std::vector<std::string>&,
           std::vector<StorageObjectMetadata>&) {
            return ErrorCode::OK;
        });
    ASSERT_TRUE(result.has_value());

    // Load and verify concatenated value
    auto load_result = LoadSingle(key);
    ASSERT_TRUE(load_result.has_value());
    EXPECT_EQ(load_result.value(), part1 + part2 + part3);
}

// ============================================================================
// Test Failure Predicate Tests
// ============================================================================

TEST_F(NvmeKvStorageBackendTest, TestFailurePredicatePartialSuccess) {
    CreateBackend();
    ASSERT_TRUE(backend_->Init().has_value());

    // Set up failure predicate to fail key2
    backend_->SetTestFailurePredicate([](const std::string& key) {
        return key == "key2";
    });

    std::unordered_map<std::string, std::vector<Slice>> batch;
    std::string v1 = "value1", v2 = "value2", v3 = "value3";
    batch["key1"] = {Slice{const_cast<char*>(v1.data()), v1.size()}};
    batch["key2"] = {Slice{const_cast<char*>(v2.data()), v2.size()}};
    batch["key3"] = {Slice{const_cast<char*>(v3.data()), v3.size()}};

    auto result = backend_->BatchOffload(
        batch,
        [](const std::vector<std::string>&,
           std::vector<StorageObjectMetadata>&) {
            return ErrorCode::OK;
        });

    ASSERT_TRUE(result.has_value());
    EXPECT_EQ(result.value(), 2);  // key1 and key3 succeed, key2 fails

    // Verify key1 exists
    auto exists1 = backend_->IsExist("key1");
    ASSERT_TRUE(exists1.has_value());
    EXPECT_TRUE(exists1.value());

    // Verify key2 does not exist
    auto exists2 = backend_->IsExist("key2");
    ASSERT_TRUE(exists2.has_value());
    EXPECT_FALSE(exists2.value());

    // Verify key3 exists
    auto exists3 = backend_->IsExist("key3");
    ASSERT_TRUE(exists3.has_value());
    EXPECT_TRUE(exists3.value());
}

// ============================================================================
// Thread Safety Tests
// ============================================================================

TEST_F(NvmeKvStorageBackendTest, ConcurrentOffloadAndLoad) {
    CreateBackend();
    ASSERT_TRUE(backend_->Init().has_value());

    const int num_threads = 4;
    const int keys_per_thread = 100;
    std::vector<std::thread> threads;
    std::atomic<int> success_count{0};

    // Concurrent offload
    for (int t = 0; t < num_threads; ++t) {
        threads.emplace_back([this, t, keys_per_thread, &success_count]() {
            for (int i = 0; i < keys_per_thread; ++i) {
                std::string key = "thread_" + std::to_string(t) + 
                                 "_key_" + std::to_string(i);
                std::string value = "thread_" + std::to_string(t) + 
                                   "_value_" + std::to_string(i);
                auto result = OffloadSingle(key, value);
                if (result.has_value()) {
                    success_count++;
                }
            }
        });
    }

    for (auto& t : threads) {
        t.join();
    }

    EXPECT_EQ(success_count.load(), num_threads * keys_per_thread);

    // Verify all keys exist
    threads.clear();
    std::atomic<int> found_count{0};

    for (int t = 0; t < num_threads; ++t) {
        threads.emplace_back([this, t, keys_per_thread, &found_count]() {
            for (int i = 0; i < keys_per_thread; ++i) {
                std::string key = "thread_" + std::to_string(t) + 
                                 "_key_" + std::to_string(i);
                auto exists = backend_->IsExist(key);
                if (exists.has_value() && exists.value()) {
                    found_count++;
                }
            }
        });
    }

    for (auto& t : threads) {
        t.join();
    }

    EXPECT_EQ(found_count.load(), num_threads * keys_per_thread);
}

// ============================================================================
// KeyID Hash Tests
// ============================================================================

TEST_F(NvmeKvStorageBackendTest, DifferentKeysDifferentKeyIds) {
    // Verify that different user keys produce different KeyIDs
    NvmeKvKeyId id1("key_alpha");
    NvmeKvKeyId id2("key_beta");
    NvmeKvKeyId id3("key_alpha");  // Same as id1

    EXPECT_NE(id1, id2);
    EXPECT_EQ(id1, id3);
}

TEST_F(NvmeKvStorageBackendTest, KeyIdWithSalt) {
    uint8_t salt1[16] = {1,2,3,4,5,6,7,8,9,10,11,12,13,14,15,16};
    uint8_t salt2[16] = {16,15,14,13,12,11,10,9,8,7,6,5,4,3,2,1};

    NvmeKvKeyId id1(salt1, 16, "same_key");
    NvmeKvKeyId id2(salt2, 16, "same_key");
    NvmeKvKeyId id3(salt1, 16, "same_key");  // Same salt and key as id1

    EXPECT_NE(id1, id2);  // Different salt
    EXPECT_EQ(id1, id3);  // Same salt and key
}

TEST_F(NvmeKvStorageBackendTest, KeyIdHexString) {
    NvmeKvKeyId id("test_key");
    std::string hex = id.to_hex_string();
    
    EXPECT_EQ(hex.size(), 32u);  // 16 bytes = 32 hex chars
    
    // Verify all chars are valid hex
    for (char c : hex) {
        EXPECT_TRUE((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'));
    }
}

// ============================================================================
// Empty and Edge Case Tests
// ============================================================================

TEST_F(NvmeKvStorageBackendTest, EmptyKey) {
    CreateBackend();
    ASSERT_TRUE(backend_->Init().has_value());

    std::string key = "";
    std::string value = "value_for_empty_key";

    auto result = OffloadSingle(key, value);
    ASSERT_TRUE(result.has_value());

    auto load_result = LoadSingle(key);
    ASSERT_TRUE(load_result.has_value());
    EXPECT_EQ(load_result.value(), value);
}

TEST_F(NvmeKvStorageBackendTest, EmptyValue) {
    CreateBackend();
    ASSERT_TRUE(backend_->Init().has_value());

    std::string key = "key_with_empty_value";
    std::string value = "";

    auto result = OffloadSingle(key, value);
    ASSERT_TRUE(result.has_value());

    auto load_result = LoadSingle(key);
    ASSERT_TRUE(load_result.has_value());
    EXPECT_EQ(load_result.value(), value);
}

TEST_F(NvmeKvStorageBackendTest, LongKeyName) {
    CreateBackend();
    ASSERT_TRUE(backend_->Init().has_value());

    // Create a key longer than 16 bytes (NVMe KV KeyID limit)
    std::string key(256, 'k');
    std::string value = "value_for_long_key";

    auto result = OffloadSingle(key, value);
    ASSERT_TRUE(result.has_value());

    auto load_result = LoadSingle(key);
    ASSERT_TRUE(load_result.has_value());
    EXPECT_EQ(load_result.value(), value);
}

TEST_F(NvmeKvStorageBackendTest, EmptyBatchOffload) {
    CreateBackend();
    ASSERT_TRUE(backend_->Init().has_value());

    std::unordered_map<std::string, std::vector<Slice>> empty_batch;

    auto result = backend_->BatchOffload(
        empty_batch,
        [](const std::vector<std::string>&,
           std::vector<StorageObjectMetadata>&) {
            return ErrorCode::OK;
        });

    ASSERT_TRUE(result.has_value());
    EXPECT_EQ(result.value(), 0);
}

}  // namespace test
}  // namespace mooncake

int main(int argc, char** argv) {
    ::testing::InitGoogleTest(&argc, argv);
    return RUN_ALL_TESTS();
}
