// Android update_engine payload.bin parser + full-OTA extractor.
//
// Format (A/B OTA, version 2):
//   magic "CrAU" | u64 version | u64 manifest_size | u32 metadata_sig_size
//   | manifest (protobuf DeltaArchiveManifest) | metadata sig | data blobs...
// For a full OTA every partition is rebuilt from REPLACE / REPLACE_XZ /
// REPLACE_BZ / ZERO operations whose data blobs live after the manifest.
#pragma once
#include "common/bytes.h"
#include "ota/zip_reader.h"
#include <cstdint>
#include <string>
#include <vector>

namespace hw::ota {

enum class OpType {
    Replace = 0, ReplaceBz = 1, Move = 2, Bsdiff = 3, SourceCopy = 4,
    SourceBsdiff = 5, Zero = 6, Discard = 7, ReplaceXz = 8, Puffdiff = 9,
    BrotliBsdiff = 10, Unknown = 255,
};
const char* op_type_name(OpType t);

struct Extent { uint64_t start_block = 0; uint64_t num_blocks = 0; };

struct InstallOp {
    OpType type = OpType::Unknown;
    uint64_t data_offset = 0;   // relative to data-blob region start
    uint64_t data_length = 0;
    std::vector<Extent> dst_extents;
};

struct Partition {
    std::string name;
    uint64_t size = 0;          // new_partition_info.size
    Bytes sha256;
    std::vector<InstallOp> ops;
};

// DeltaArchiveManifest.dynamic_partition_metadata (field 15): the update_engine
// description of the logical partitions that live inside the physical `super`.
// The OTA ships no super.img -- update_engine (and now we) build super from the
// per-partition images plus this group/partition layout.
struct DynamicPartitionGroup {
    std::string name;                        // e.g. "hollywood_dynamic_partitions_a"
    uint64_t maximum_size = 0;
    std::vector<std::string> partition_names; // e.g. {system, system_ext, vendor, ...}
};
struct DynamicPartitionMetadata {
    std::vector<DynamicPartitionGroup> groups;
    bool present = false;
};

struct Payload {
    uint64_t version = 0;
    uint32_t block_size = 4096;
    uint64_t header_size = 0;
    uint64_t manifest_size = 0;
    uint64_t metadata_sig_size = 0;
    uint64_t data_blob_start = 0;   // offset within payload.bin
    uint64_t payload_offset_in_zip = 0; // absolute offset of payload.bin in the OTA zip
    std::vector<Partition> partitions;
    DynamicPartitionMetadata dap;   // dynamic-partition (super) layout

    const Partition* find(const std::string& name) const;
};

// Parse only the header + manifest (cheap; reads ~90KB, not the 1GB of blobs).
Payload parse_payload(const ZipReader& zip);

// Reconstruct one partition into a contiguous buffer by applying its ops.
Bytes extract_partition(const ZipReader& zip, const Payload& pl, const std::string& name);

} // namespace hw::ota
