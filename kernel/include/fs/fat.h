#ifndef FAT_H
#define FAT_H

#include <stdint.h>

// to avoid including libraries
typedef uint8_t bool;
#define true 1
#define false 0
#define NULL (void *)0
typedef uint32_t size_t;

typedef struct {
    // 
    uint8_t jmp[3];
    uint8_t oem[8];
    uint16_t bytes_per_sector;
    uint8_t sectors_per_cluster;
    uint16_t reserved_sectors;
    uint8_t fat_count;
    uint16_t dir_entry_count;
    uint16_t total_sectors;
    uint8_t media;
    uint16_t sectors_per_fat;
    uint16_t sectors_per_track;
    uint16_t heads;
    uint32_t hidden_sectors;
    uint32_t large_sector_count;

    // ebr (extended boot record)
    uint8_t drive_number;
    uint8_t reserved;
    uint8_t signature;
    uint32_t volume_id;
    uint8_t volume_label[11];
    uint8_t system_id[8];

} __attribute__((packed)) boot_sector_t;

typedef struct {
    uint8_t name[11];
    uint8_t attributes;
    uint32_t size;
    uint8_t reserved;
    uint16_t created_time;
    uint16_t created_date;
    uint8_t created_time_tenths;
    uint16_t accessed_date;
    uint16_t modified_time;
    uint16_t modified_date;
    uint16_t first_cluster_low;
    uint16_t first_cluster_high;
} __attribute__((packed)) directory_entry_t;

typedef struct {
    void* buffer;   // Pointer to the data
    size_t size;    // Size of the data
    size_t position;  // Current read position
} file_t;

#endif  // FAT_H