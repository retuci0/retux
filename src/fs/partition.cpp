#include "fs/partition.hpp"
#include "lib/string.hpp"
#include "io/serial.hpp"


namespace partition {

    // MBR structs
    struct MBRPartition {
        u8 status;
        u8 chs_first[3];
        u8 type;
        u8 chs_last[3];
        u32 lba_start;
        u32 sector_count;
    } __attribute__((packed));

    struct MBR {
        u8 bootcode[446];
        MBRPartition parts[4];
        u16 signature;  // 0xAA55
    } __attribute__((packed));

    // GPT structs
    struct GPTHeader {
        u8 signature[8];  // "EFI PART"
        u32 revision;
        u32 header_size;
        u32 header_crc32;
        u32 reserved;
        u64 current_lba;
        u64 backup_lba;
        u64 first_usable_lba;
        u64 last_usable_lba;
        u8 disk_guid[16];
        u64 partition_entry_lba;
        u32 num_entries;
        u32 entry_size;
        u32 entry_crc32;
        // more but nah
    } __attribute__((packed));

    struct GPTEntry {
        u8 partition_type_guid[16];
        u8 unique_guid[16];
        u64 first_lba;
        u64 last_lba;
        u64 attributes;
        u16 name[36];  // UTF-16LE
    } __attribute__((packed));

    int scan(vfs::ReadBlock raw_read, Partition* out, int max) {
        if (!raw_read || max <= 0) return 0;
        u8 sector[512];
        if (!raw_read(0, 1, sector)) {
            serial::print("partition: failed to read LBA 0\n");
            return 0;
        }
        const MBR* mbr = reinterpret_cast<const MBR*>(sector);
        if (mbr->signature != 0xAA55) {
            serial::print("partition: invalid MBR signature\n");
            return 0;
        }

        // check for protective MBR (GPT) - partition type 0xEE
        bool is_gpt = false;
        for (int i = 0; i < 4; ++i) {
            if (mbr->parts[i].type == 0xEE) {
                is_gpt = true;
                break;
            }
        }
        if (is_gpt) {
            serial::print("partition: GPT detected, parsing...\n");
            // read GPT header at LBA 1
            if (!raw_read(1, 1, sector)) {
                serial::print("partition: failed to read GPT header\n");
                return 0;
            }
            const GPTHeader* gpt = reinterpret_cast<const GPTHeader*>(sector);
            // check signature
            if (string::strncmp((const char*)gpt->signature, "EFI PART", 8) != 0) {
                serial::print("partition: invalid GPT signature\n");
                return 0;
            }
            // read partition entries (assume entry_size >= 128)
            u32 entry_size = gpt->entry_size;
            if (entry_size < 128) entry_size = 128;
            u32 entries_per_sector = 512 / entry_size;
            u64 entry_lba = gpt->partition_entry_lba;
            int count = 0;
            for (u32 i = 0; i < gpt->num_entries && count < max; ++i) {
                u32 sector_offset = i / entries_per_sector;
                u32 entry_offset = i % entries_per_sector;
                u8 sector_buf[512];
                if (!raw_read(entry_lba + sector_offset, 1, sector_buf)) {
                    serial::print("partition: failed to read GPT entry sector\n");
                    break;
                }
                const GPTEntry* e = reinterpret_cast<const GPTEntry*>(sector_buf + entry_offset * entry_size);
                // check if partition is unused (all zero GUID)
                bool zero = true;
                for (int j = 0; j < 16; ++j) if (e->partition_type_guid[j] != 0) { zero = false; break; }
                if (zero) continue;
                // could map type to a byte, but for now just store 0 and check later via ext2 magic
                out[count].start_lba = e->first_lba;
                out[count].sector_count = e->last_lba - e->first_lba + 1;
                out[count].type = 0;  // not used
                out[count].valid = true;
                count++;
            }
            serial::print("partition: found "); serial::print_dec(count); serial::print(" GPT partitions\n");
            return count;
        }

        // MBR (non GPT)
        int count = 0;
        for (int i = 0; i < 4 && count < max; ++i) {
            const auto& p = mbr->parts[i];
            if (p.type == 0) continue;  // empty
            out[count].start_lba = p.lba_start;
            out[count].sector_count = p.sector_count;
            out[count].type = p.type;
            out[count].valid = true;
            count++;
        }
        serial::print("partition: found "); serial::print_dec(count); serial::print(" MBR partitions\n");
        return count;
    }

    bool read_partition(u64 lba, u32 count, void* buf, void* user_data) {
        PartitionReadContext* ctx = (PartitionReadContext*)user_data;
        return ctx->raw_read(ctx->base_lba + lba, count, buf);
    }

}
