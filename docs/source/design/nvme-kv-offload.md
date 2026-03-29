# NVMe KV SSD Backend Design Document

## Overview
This document presents the design for NVMe Key-Value (KV) SSD backend, correcting prior misconceptions regarding NVMe KV. It is important to clarify that NVMe KV is not a network protocol; rather, it functions as a direct interface for storage management.

## Key Definitions
- **KeyID Mapping**: Each `user_key` is mapped to a 16B KeyID using a hashing mechanism:  
  `KeyID = Hash128(salt || user_key)`  
  This ensures uniqueness and scalability for key management.

- **Value Payload Structure**: To ensure collision detection and proper storage management, the `user_key` is stored within the value payload structured as follows:  
  `[u32 key_len | u32 value_len | key | value]`
  This aligns with the design principles of OffsetAllocatorStorageBackend.

## Abstractions and Integration
- **NvmeKvDevice Abstraction**: The design also introduces the `NvmeKvDevice` abstraction, facilitating efficient interactions with the storage layer.
- **StorageBackend Integration**: The `NvmeKvStorageBackend` integrates with `StorageBackendInterface`, allowing for seamless operations and consistent API usage across different storage backends.
- **ScanMeta**: Utilizing a host index for efficient scanning operations, `ScanMeta` is designed to enhance data retrieval performance.

## Persistence and Eviction Protocol
- **Write-Ahead Logging (WAL) and Checkpointing**: Our design implements a robust persistence mechanism through WAL and checkpointing, ensuring data integrity during failures.
- **Eviction Protocol**: A two-phase eviction protocol is established to manage stored entries effectively, preventing memory overconsumption.

## Configuration Variables
The following configuration variables are essential for the deployment and tuning of the NVMe KV SSD backend:
- `max_entries`: Maximum number of key-value entries supported.
- `eviction_threshold`: The threshold at which the eviction process triggers.
- `wal_enabled`: Boolean flag to enable/disable write-ahead logging.

This design aims to set a foundation for building an efficient NVMe KV SSD backend that fully utilizes modern storage capabilities while addressing key operational considerations.