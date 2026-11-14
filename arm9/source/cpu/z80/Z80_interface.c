// =====================================================================================
// Copyright (c) 2026 Dave Bernazzani (wavemotion-dave)
//
// Copying and distribution of this emulator, its source code and associated
// readme files, with or without modification, are permitted in any medium without
// royalty provided this copyright notice is used and wavemotion-dave and
// Marat Fayzullin (fMSX core) are thanked profusely.
//
// The Vroom-DS emulator is offered as-is, without any warranty. Please see readme.md
//
// This file is our bridge between the Z80 CPU core and the rest of the system.
// VroomDS currently supports the CZ80 CPU core.
// =====================================================================================
#include <nds.h>

#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "Z80_interface.h"
#include "../../Vroom-DS.h"
#include "../../MSX_generic.h"
#include "../../fdc.h"
#include "../../printf.h"

u8 PAC_SRAM_in_view     = 0; // Is the PAC SRAM in view?
u8 FMPAC_Key1           = 0; // FM-PAC SRAM requires a special key write sequence to open up SRAM access

// Some mapper storage for the really BIG mapper carts like ASCII16-X and NEO8/16 which can store more data
u8 ascii16xFlashCmdState[3] = {0};
u8 ascii16xFlashReadMode[3] = {0};
u8 ascii16xCfiBackup[3][ASCII16X_CFI_SIZE];
u16 NEO8_bank[6] = {0};
u16 NEO16_bank[3] = {0};

extern u8 Unmapped_Memory[];

// -----------------------------------------------------------------------------------------------
// All memory fetches run through this except OP codes which are read directly from memory
// for maximum speed (an Op Code fetch should never result in accessing special memory registers).
// -----------------------------------------------------------------------------------------------
ITCM_CODE u8 cpu_readmem16(u16 address)
{
    if ((special_memory_access & SPEC_MEM_DISK_CONTROLLER) && myConfig.fdc_driver == 1 && (address >= 0x7FF0) && (address <= 0x7FFF))
    {
        return fdc_read(address & 0x0F);
    }
    else if ((special_memory_access & SPEC_MEM_DISK_CONTROLLER) && (address >= 0x7FF8) && (address <= 0x7FFF))
    {
        if (address == 0x7FFF) return fdc_read(4);
        else return fdc_read(address & 7);
    }
    else if ((special_memory_access & (SPEC_MEM_SUBSLOT0_ACTIVE | SPEC_MEM_SUBSLOT3_ACTIVE)) && (address == 0xFFFF)) // Subslot check... only when page 3 is mapped to an expanded slot
    {
        if (special_memory_access & SPEC_MEM_SUBSLOT0_ACTIVE)
            return (u8)~msx_subslot0; // Compliment is returned.
        else
            return (u8)~msx_subslot3;  // Compliment is returned.
    }

    // Otherwise normal read - just index into the 8K memory block and fetch the byte...
    return (MemoryMap[address>>13][address]);
}


// ---------------------------------------------------------------------------------
// The Konami 8K Mapper without SCC:
// 4000h-5FFFh - fixed ROM area (not swappable)
// 6000h~7FFFh (mirror: E000h~FFFFh)    6000h (mirrors: 6001h~7FFFh)    1
// 8000h~9FFFh (mirror: 0000h~1FFFh)    8000h (mirrors: 8001h~9FFFh)    Random
// A000h~BFFFh (mirror: 2000h~3FFFh)    A000h (mirrors: A001h~BFFFh)    Random
// ---------------------------------------------------------------------------------
void __attribute__((noinline)) HandleKonami8(u8 media_id, u16 address, u8 value)
{
    u32 block = (value & MyMedia[media_id].mapperMask);
    u32 msx_offset = block * MyMedia[media_id].blockSize;
    u32 *src = (u32*)((u8*)ROM_Memory + msx_offset + (media_id ? ((MAX_CART_SIZE_KB/2) * 1024) : 0));

    if ((address & 0xE000) == 0x6000)
    {
        MSXCartPtr[media_id][3] = (u8*)src;  // Main ROM
        MSXCartPtr[media_id][7] = (u8*)src;  // Mirror
        MemoryMap[3] = (u8 *)(MSXCartPtr[media_id][3]) - 0x6000;
    }
    else if ((address & 0xE000) == 0x8000)
    {
        MSXCartPtr[media_id][4] = (u8*)src;  // Main ROM
        MSXCartPtr[media_id][0] = (u8*)src;  // Mirror
        MemoryMap[4] = (u8 *)(MSXCartPtr[media_id][4]) - 0x8000;
    }
    else if ((address & 0xE000) == 0xA000)
    {
        MSXCartPtr[media_id][5] = (u8*)src;  // Main ROM
        MSXCartPtr[media_id][1] = (u8*)src;  // Mirror
        MemoryMap[5] = (u8 *)(MSXCartPtr[media_id][5]) - 0xA000;
    }
}

// -------------------------------------------------------------------------
// The ASCII 8K Mapper:
// 4000h~5FFFh (mirror: C000h~DFFFh)    6000h (mirrors: 6001h~67FFh)    0
// 6000h~7FFFh (mirror: E000h~FFFFh)    6800h (mirrors: 6801h~68FFh)    0
// 8000h~9FFFh (mirror: 0000h~1FFFh)    7000h (mirrors: 7001h~77FFh)    0
// A000h~BFFFh (mirror: 2000h~3FFFh)    7800h (mirrors: 7801h~7FFFh)    0
// -------------------------------------------------------------------------
void __attribute__((noinline)) HandleAscii8(u8 media_id, u16 address, u8 value)
{
    u32 block = (value & MyMedia[media_id].mapperMask);
    u32 msx_offset = block * MyMedia[media_id].blockSize;
    u32 *src = (u32*)((u8*)ROM_Memory + msx_offset + (media_id ? ((MAX_CART_SIZE_KB/2) * 1024) : 0));

    if ((address & 0xF800) == 0x6000)
    {
        MSXCartPtr[media_id][2] = (u8*)src;  // Main ROM
        MSXCartPtr[media_id][6] = (u8*)src;  // Mirror
        MemoryMap[2] = MSXCartPtr[media_id][2] - 0x4000;
        if (bCartInPage[3] == (media_id+1))
        {
            MemoryMap[6] = MSXCartPtr[media_id][6] - 0xC000;
        }
    }
    else if ((address & 0xF800) == 0x6800)
    {
        MSXCartPtr[media_id][3] = (u8*)src;  // Main ROM
        MSXCartPtr[media_id][7] = (u8*)src;  // Mirror
        MemoryMap[3] = MSXCartPtr[media_id][3] - 0x6000;
        if (bCartInPage[3] == (media_id+1))
        {
            MemoryMap[7] = MSXCartPtr[media_id][7] - 0xE000;
        }
    }
    else if ((address & 0xF800) == 0x7000)
    {
        MSXCartPtr[media_id][4] = (u8*)src;  // Main ROM
        MSXCartPtr[media_id][0] = (u8*)src;  // Mirror
        if (bCartInPage[2] == (media_id+1))
        {
            MemoryMap[4] = MSXCartPtr[media_id][4] - 0x8000;
        }
        if (bCartInPage[0] == (media_id+1))
        {
            MemoryMap[0] = MSXCartPtr[media_id][0] - 0x0000;
        }
    }
    else if ((address & 0xF800) == 0x7800)
    {
        MSXCartPtr[media_id][5] = (u8*)src;  // Main ROM
        MSXCartPtr[media_id][1] = (u8*)src;  // Mirror
        if (bCartInPage[2] == (media_id+1))
        {
            MemoryMap[5] = MSXCartPtr[media_id][5] - 0xA000;
        }
        if (bCartInPage[0] == (media_id+1))
        {
            MemoryMap[1] = MSXCartPtr[media_id][1] - 0x2000;
        }
    }
}


// -------------------------------------------------------------------------
// The ASCII 16K Mapper:
// 4000h~7FFFh  via writes to 6000h to 67FFh
// 8000h~BFFFh  via writes to 7000h to 77FFh
// -------------------------------------------------------------------------
void __attribute__((noinline)) HandleAscii16(u8 media_id, u16 address, u8 value)
{
    u32 block = (value & MyMedia[media_id].mapperMask);
    u32 msx_offset = block * MyMedia[media_id].blockSize;
    u32 *src = (u32*)((u8*)ROM_Memory + msx_offset + (media_id ? ((MAX_CART_SIZE_KB/2) * 1024) : 0));

    if ((address & 0xF800) == 0x6000)
    {
        MSXCartPtr[media_id][2] = (u8*)src;
        MSXCartPtr[media_id][3] = (u8*)src+0x2000;
        MemoryMap[2] = MSXCartPtr[media_id][2] - 0x4000;
        MemoryMap[3] = MSXCartPtr[media_id][3] - 0x6000;
    }
    else if ((address & 0xF800) == 0x7000)
    {
        MSXCartPtr[media_id][4] = (u8*)src;
        MSXCartPtr[media_id][5] = (u8*)src+0x2000;
        if (bCartInPage[2] == (media_id+1))
        {
            MemoryMap[4] = MSXCartPtr[media_id][4] - 0x8000;
            MemoryMap[5] = MSXCartPtr[media_id][5] - 0xA000;
        }
    }
}

// -------------------------------------------------------------------------
// The ASCII 16K Mapper on steroids... has mirrors and can allow 4MB+ ROMs.
// -------------------------------------------------------------------------
static const u16 ascii16xAutoSelectData[16] =
{
    0x0001, 0x227E, 0x0000, 0xFF0A,
    0xFFFF, 0xFFFF, 0xFFFF, 0xFFFF,
    0xFFFF, 0xFFFF, 0xFFFF, 0xFFFF,
    0xFFFF, 0xFFFF, 0x2210, 0x2200
};
static const u16 ascii16xCfiIdData[16] =
{
    0x0001, 0x227E, 0xFFFF, 0xFF0A,
    0xFFFF, 0xFFFF, 0xFFFF, 0xFFFF,
    0xFFFF, 0xFFFF, 0xFFFF, 0xFFFF,
    0xFFFF, 0xFFFF, 0x2210, 0x2200
};

void __attribute__((noinline)) HandleAscii16X(u8 media_id, u16 address, u8 value)
{
    // ASCII-16X: bank number uses address bits A11-A8 plus the data byte.
    // The mapper mask limits this to the ROM size supported by this cartridge.
    // We support 4MB and 8MB here only...
    u32 block = ((address & 0x0F00) | value) & MyMedia[media_id].mapperMask;
    u32 msx_offset = block * MyMedia[media_id].blockSize;
    u8 *src = (u8*)ROM_Memory + msx_offset + (media_id ? ((MAX_CART_SIZE_KB / 2) * 1024) : 0);

    u32 cart_capacity = media_id ? ((MAX_CART_SIZE_KB / 2) * 1024) : (MAX_CART_SIZE_KB * 1024);
    u32 flash_size = MyMedia[media_id].filesize;
    if (flash_size > cart_capacity)
        flash_size = cart_capacity;
    if (flash_size > (8 * 1024 * 1024))
        flash_size = 8 * 1024 * 1024;
    u8 *flash_base = (u8*)ROM_Memory + (media_id ? ((MAX_CART_SIZE_KB / 2) * 1024) : 0);

    // Query modes temporarily replace the start of the flash image, allowing
    // ordinary memory reads to return the identification data directly.
    if (ascii16xFlashReadMode[media_id])
    {
        if (value == 0xF0)
        {
            memcpy(flash_base, ascii16xCfiBackup[media_id], ASCII16X_CFI_SIZE);
            ascii16xFlashReadMode[media_id] = 0;
            ascii16xFlashCmdState[media_id] = 0;
        }
    }
    else if ((value == 0xF0) && (ascii16xFlashCmdState[media_id] != 3))
    {
        ascii16xFlashCmdState[media_id] = 0;
    }
    else if ((value == 0x98) && ((address & 0x0FFF) == 0x00AA) &&
             (flash_size >= ASCII16X_CFI_SIZE) && (ascii16xFlashCmdState[media_id] != 3))
    {
        u8 cfi[ASCII16X_CFI_SIZE] = {0};
        u32 sector_count = (flash_size > 0x10000) ? ((flash_size - 0x10000) / 0x10000) : 0;
        u32 size_bytes = 1;
        u8 size_exponent = 0;

        while ((size_bytes < flash_size) && (size_exponent < 31))
        {
            size_bytes <<= 1;
            size_exponent++;
        }

        for (u32 i = 0; i < 16; i++)
        {
            u16 word = ascii16xCfiIdData[i];
            cfi[ASCII16X_CFI_BYTE(i)] = word;
            cfi[ASCII16X_CFI_BYTE(i) + 1] = word >> 8;
        }

        cfi[ASCII16X_CFI_BYTE(0x10)] = 'Q';
        cfi[ASCII16X_CFI_BYTE(0x11)] = 'R';
        cfi[ASCII16X_CFI_BYTE(0x12)] = 'Y';
        cfi[ASCII16X_CFI_BYTE(0x13)] = 0x02; // AMD/Fujitsu command set.
        cfi[ASCII16X_CFI_BYTE(0x15)] = 0x40; // Primary extended query table.
        cfi[ASCII16X_CFI_BYTE(0x1B)] = 0x27; // Vcc range: 2.7V to 3.6V.
        cfi[ASCII16X_CFI_BYTE(0x1C)] = 0x36;
        cfi[ASCII16X_CFI_BYTE(0x1F)] = 0x04; // Typical byte-program timeout: 16 us.
        cfi[ASCII16X_CFI_BYTE(0x20)] = 0x0C;
        cfi[ASCII16X_CFI_BYTE(0x23)] = 0x0A; // Typical sector-erase timeout: 1 s.
        cfi[ASCII16X_CFI_BYTE(0x24)] = 0x03;
        cfi[ASCII16X_CFI_BYTE(0x27)] = size_exponent;
        cfi[ASCII16X_CFI_BYTE(0x2C)] = (sector_count > 0) ? 2 : 1;
        cfi[ASCII16X_CFI_BYTE(0x2D)] = 7;    // Eight 8K boot sectors.
        cfi[ASCII16X_CFI_BYTE(0x2F)] = 0x20; // 8K in units of 256 bytes.

        if (sector_count > 0)
        {
            u32 large_sectors_minus_one = sector_count - 1;
            cfi[ASCII16X_CFI_BYTE(0x31)] = large_sectors_minus_one & 0xFF;
            cfi[ASCII16X_CFI_BYTE(0x32)] = (large_sectors_minus_one >> 8) & 0xFF;
            cfi[ASCII16X_CFI_BYTE(0x33)] = 0x00;
            cfi[ASCII16X_CFI_BYTE(0x34)] = 0x01; // 64K in units of 256 bytes.
        }

        cfi[ASCII16X_CFI_BYTE(0x40)] = 'P';
        cfi[ASCII16X_CFI_BYTE(0x41)] = 'R';
        cfi[ASCII16X_CFI_BYTE(0x42)] = 'I';
        cfi[ASCII16X_CFI_BYTE(0x43)] = '1';
        cfi[ASCII16X_CFI_BYTE(0x44)] = '3';

        memcpy(ascii16xCfiBackup[media_id], flash_base, ASCII16X_CFI_SIZE);
        memcpy(flash_base, cfi, ASCII16X_CFI_SIZE);
        ascii16xFlashReadMode[media_id] = 1;
        ascii16xFlashCmdState[media_id] = 0;
    }
    else
    {
        u16 command_address = ASCII16X_COMMAND_ADDRESS(address);
        u8 *mapped = MSXCartPtr[media_id][address >> 13];
        uintptr_t cart_start = (uintptr_t)flash_base;
        uintptr_t dest = (uintptr_t)mapped + (address & 0x1FFF);

        switch (ascii16xFlashCmdState[media_id])
        {
            case 0:
                if ((command_address == 0x0AAA) && (value == 0xAA))
                    ascii16xFlashCmdState[media_id] = 1;
                break;

            case 1:
                if ((command_address == 0x0554) && (value == 0x55))
                    ascii16xFlashCmdState[media_id] = 2;
                else
                    ascii16xFlashCmdState[media_id] = ((command_address == 0x0AAA) && (value == 0xAA)) ? 1 : 0;
                break;

            case 2:
                if ((command_address == 0x0AAA) && (value == 0x90))
                {
                    memcpy(ascii16xCfiBackup[media_id], flash_base, ASCII16X_CFI_SIZE);
                    for (u32 i = 0; i < ASCII16X_CFI_SIZE; i++)
                    {
                        u16 word = ascii16xAutoSelectData[(i >> 1) & 0x0F];
                        flash_base[i] = (i & 1) ? (word >> 8) : word;
                    }
                    ascii16xFlashReadMode[media_id] = 2;
                    ascii16xFlashCmdState[media_id] = 0;
                }
                else if ((command_address == 0x0AAA) && (value == 0xA0))
                    ascii16xFlashCmdState[media_id] = 3;
                else if ((command_address == 0x0AAA) && (value == 0x80))
                    ascii16xFlashCmdState[media_id] = 4;
                else
                    ascii16xFlashCmdState[media_id] = 0;
                break;

            case 3: // Program one byte.
                if ((mapped != NULL) && (dest >= cart_start) && (dest - cart_start < flash_size))
                {
                    u8 *flash_byte = (u8*)dest;
                    *flash_byte &= value;
                    flashDirtyBlocks[(dest - cart_start) / FLASH_DIRTY_BLOCK_SIZE] = 1;
                    flash_show_status = 4;
                }
                ascii16xFlashCmdState[media_id] = 0;
                break;

            case 4: // Erase setup: second unlock sequence.
                if ((command_address == 0x0AAA) && (value == 0xAA))
                    ascii16xFlashCmdState[media_id] = 5;
                else
                    ascii16xFlashCmdState[media_id] = 0;
                break;

            case 5:
                if ((command_address == 0x0554) && (value == 0x55))
                    ascii16xFlashCmdState[media_id] = 6;
                else
                    ascii16xFlashCmdState[media_id] = 0;
                break;

            case 6: // Sector erase.
                if ((value == 0x30) && (mapped != NULL) &&
                    (dest >= cart_start) && (dest - cart_start < flash_size))
                {
                    u32 offset = (u32)(dest - cart_start);
                    u32 sector_start = (offset < 0x10000)
                        ? (offset & ~0x1FFF)
                        : (0x10000 + ((offset - 0x10000) & ~0xFFFF));
                    u32 sector_size = (sector_start < 0x10000) ? 0x2000 : 0x10000;
                    if (sector_size > flash_size - sector_start)
                        sector_size = flash_size - sector_start;

                    memset(flash_base + sector_start, 0xFF, sector_size);
                    u32 first_dirty_block = sector_start / FLASH_DIRTY_BLOCK_SIZE;
                    u32 last_dirty_block = (sector_start + sector_size - 1) / FLASH_DIRTY_BLOCK_SIZE;
                    for (u32 dirty_block = first_dirty_block; dirty_block <= last_dirty_block; dirty_block++)
                    {
                        flashDirtyBlocks[dirty_block] = 1;
                    }
                    flash_show_status = 4;
                }
                ascii16xFlashCmdState[media_id] = 0;
                break;

            default:
                ascii16xFlashCmdState[media_id] = 0;
                break;
        }
    }

    // OpenMSX accepts writes when A13 is set within either 16 KB region.
    if ((address & 0x3FFF) >= 0x2000)
    {
        // A12 selects the bank register.
        if (((address >> 12) & 1) == 0)
        {
            // First 16 KB window: 4000-7FFF, mirrored at C000-FFFF.
            MSXCartPtr[media_id][2] = src;
            MSXCartPtr[media_id][3] = src + 0x2000;

            MSXCartPtr[media_id][6] = src;
            MSXCartPtr[media_id][7] = src + 0x2000;

            if (bCartInPage[1] == (media_id + 1))
            {
                MemoryMap[2] = MSXCartPtr[media_id][2] - 0x4000;
                MemoryMap[3] = MSXCartPtr[media_id][3] - 0x6000;
            }

            if (bCartInPage[3] == (media_id + 1))
            {
                MemoryMap[6] = MSXCartPtr[media_id][6] - 0xC000;
                MemoryMap[7] = MSXCartPtr[media_id][7] - 0xE000;
            }
        }

        #undef ASCII16X_CFI_BYTE
        #undef ASCII16X_COMMAND_ADDRESS
        else
        {
            // Second 16 KB window: 8000-BFFF, mirrored at 0000-3FFF.
            MSXCartPtr[media_id][4] = src;
            MSXCartPtr[media_id][5] = src + 0x2000;

            MSXCartPtr[media_id][0] = src;
            MSXCartPtr[media_id][1] = src + 0x2000;

            if (bCartInPage[2] == (media_id + 1))
            {
                MemoryMap[4] = MSXCartPtr[media_id][4] - 0x8000;
                MemoryMap[5] = MSXCartPtr[media_id][5] - 0xA000;
            }

            if (bCartInPage[0] == (media_id + 1))
            {
                MemoryMap[0] = MSXCartPtr[media_id][0] - 0x0000;
                MemoryMap[1] = MSXCartPtr[media_id][1] - 0x2000;
            }
        }
    }
}


// --------------------------------------------------------------------------------
// NEO8 is an 8K mapper but has a 16-bit block size. We can only support 8MB max...
// --------------------------------------------------------------------------------
void __attribute__((noinline)) HandleNeo8(u8 media_id, u16 address, u8 value)
{
    u8 bank = ((address >> 11) & 0x07) - 2;   // 0..5 valid, anything else wraps to >5
    if (bank > 5)
        return;

    if (address & 1)   // odd = high byte (only 6 bits used as upper bits reserved)
        NEO8_bank[bank] = (NEO8_bank[bank] & 0x00FF) | ((u16)(value & 0x3F) << 8);
    else               // even = low byte
        NEO8_bank[bank] = (NEO8_bank[bank] & 0xFF00) | value;

    u32 seg       = NEO8_bank[bank] & MyMedia[media_id].mapperMask;
    u32 msx_offset = seg * MyMedia[media_id].blockSize;

    u8 *src = (u8*)ROM_Memory + msx_offset;
    MSXCartPtr[media_id][bank] = src;

    if (bCartInPage[bank >> 1] == (media_id + 1))
        MemoryMap[bank] = src - (bank * 0x2000);
}


// -----------------------------------------------------------------------------------
// NEO16 is an 16K mapper but has a 16-bit block size. We can only support 8MB max...
// -----------------------------------------------------------------------------------
void __attribute__((noinline)) HandleNeo16(u8 media_id, u16 address, u8 value)
{
    // Register select = bits 13..11 (BBB): 010 -> bank 0, 100 -> bank 1, 110 -> bank 2
    u8 bbb = (address >> 11) & 0x07;
    if (bbb < 2 || (bbb & 1))
        return;
    u8 bank = (bbb - 2) >> 1;                  // 0..2

    if (address & 1)   // odd = high byte (only 6 bits used as upper bits reserved)
        NEO16_bank[bank] = (NEO16_bank[bank] & 0x00FF) | ((u16)(value & 0x3F) << 8);
    else               // even = low byte
        NEO16_bank[bank] = (NEO16_bank[bank] & 0xFF00) | value;

    // Segments are 16K here, regardless of what blockSize holds for this media
    u32 seg     = NEO16_bank[bank] & MyMedia[media_id].mapperMask;
    u8 *src     = (u8*)ROM_Memory + (seg << 14);

    // Keep the per-8K cart pointers in the same layout HandleNeo8 uses
    MSXCartPtr[media_id][bank * 2]     = src;
    MSXCartPtr[media_id][bank * 2 + 1] = src + 0x2000;

    if (bCartInPage[bank] == (media_id + 1))   // 16K bank n == MSX page n
    {
        u8 *base = src - (bank * 0x4000);      // same bias works for both 8K halves
        MemoryMap[bank * 2]     = base;
        MemoryMap[bank * 2 + 1] = base;
    }
}


// MSX DOS 2 by ASCII only maps 0x4000-0x7FFF and the hotpsot is at 0x7FFE
void __attribute__((noinline)) HandleMSXDOS2(u16 address, u8 value)
{
    dos22_bank = (value & 3);
    u8 *dos_rom = TURBO_DOS22;
    u8 *src = dos_rom + (dos22_bank * 0x4000);

    if (address == 0x7FFE) // The hot-spot for the ASCII external DOS2.2
    {
        MemoryMap[2] = (u8*)src + 0x0000 - 0x4000;
        MemoryMap[3] = (u8*)src + 0x2000 - 0x6000;
    }
}

void __attribute__((noinline)) HandleKonamiSCC8(u8 media_id, u16 address, u8 value)
{
    u32 block = (value & MyMedia[media_id].mapperMask);
    u32 msx_offset = block * MyMedia[media_id].blockSize;
    u32 *src = (u32*)((u8*)ROM_Memory + msx_offset + (media_id ? ((MAX_CART_SIZE_KB/2) * 1024) : 0));

    // -----------------------------------------------------------------------------------
    // Are we writing to the SCC chip memory mapped area and are the registers "in view"?
    // -----------------------------------------------------------------------------------
    {
        // --------------------------------------------------------
        // Konami 8K mapper with SCC
        //  Bank 1: 4000h - 5FFFh - mapped via writes to 5000h
        //  Bank 2: 6000h - 7FFFh - mapped via writes to 7000h
        //  Bank 3: 8000h - 9FFFh - mapped via writes to 9000h
        //  Bank 4: A000h - BFFFh - mapped via writes to B000h
        // --------------------------------------------------------
        if ((address & 0xF800) == 0x5000)
        {
            MSXCartPtr[media_id][2] = (u8*)src;  // Main ROM
            MSXCartPtr[media_id][6] = (u8*)src;  // Mirror
            MemoryMap[2] = (u8 *)(MSXCartPtr[media_id][2]) - 0x4000;

            if (bCartInPage[3] == (media_id+1))
            {
                MemoryMap[6] = MSXCartPtr[media_id][6] - 0xC000;
            }
        }
        else if ((address & 0xF800) == 0x7000)
        {
            MSXCartPtr[media_id][3] = (u8*)src;  // Main ROM
            MSXCartPtr[media_id][7] = (u8*)src;  // Mirror
            MemoryMap[3] = (u8 *)(MSXCartPtr[media_id][3]) - 0x6000;

            if (bCartInPage[3] == (media_id+1))
            {
                MemoryMap[7] = MSXCartPtr[media_id][7] - 0xE000;
            }
        }
        else if ((address & 0xF800) == 0x9000)
        {
            MSXCartPtr[media_id][4] = (u8*)src;  // Main ROM
            MSXCartPtr[media_id][0] = (u8*)src;  // Mirror
            MemoryMap[4] = (u8 *)(MSXCartPtr[media_id][4]) - 0x8000;

            if (bCartInPage[0] == (media_id+1))
            {
                MemoryMap[0] = MSXCartPtr[media_id][0] - 0x0000;
            }
        }
        else if ((address & 0xF800) == 0xB000)
        {
            MSXCartPtr[media_id][5] = (u8*)src;  // Main ROM
            MSXCartPtr[media_id][1] = (u8*)src;  // Mirror
            MemoryMap[5] = (u8 *)(MSXCartPtr[media_id][5]) - 0xA000;

            if (bCartInPage[0] == (media_id+1))
            {
                MemoryMap[1] = MSXCartPtr[media_id][1] - 0x2000;
            }
        }
    }
}


// --------------------------------------------------------------------
// The PAC-SRAM presents 8K of SRAM for a game to use to save progress.
// This handler is called when any byte in the 0x4000-0x5FFF range is
// written and will determine if/when the SRAM is available.
// --------------------------------------------------------------------
#define FMPAC_KEY1 0x4D
#define FMPAC_KEY2 0x69
void __attribute__((noinline)) Handle_PAC_SRAM(u8 media_id, u16 address, u8 value)
{
    extern u8* FAKE_FMPAC;
    
    if (address == 0x5FFE)
    {
        // Key register
        SRAM_Memory[0x1FFE] = value;

        if ((SRAM_Memory[0x1FFF] == 0x69) && (value == 0x4D))
        {
            PAC_SRAM_in_view = 1;
            MSXCartPtr[MEDIA_CART2][2] = (u8*)SRAM_Memory;
            MemoryMap[2] = MSXCartPtr[MEDIA_CART2][2] - 0x4000;
        }
        else
        {
            PAC_SRAM_in_view = 0;
            MSXCartPtr[MEDIA_CART2][2] = (u8*)FAKE_FMPAC;
            MemoryMap[2] = MSXCartPtr[MEDIA_CART2][2] - 0x4000;
        }
        return;
    }
    else if (address == 0x5FFF)
    {
        // Key register
        SRAM_Memory[0x1FFF] = value;

        if ((SRAM_Memory[0x1FFE] == 0x4D) && (value == 0x69))
        {
            PAC_SRAM_in_view = 1;
            MSXCartPtr[MEDIA_CART2][2] = (u8*)SRAM_Memory;
            MemoryMap[2] = MSXCartPtr[MEDIA_CART2][2] - 0x4000;
        }
        else
        {
            PAC_SRAM_in_view = 0;
            MSXCartPtr[MEDIA_CART2][2] = (u8*)FAKE_FMPAC;
            MemoryMap[2] = MSXCartPtr[MEDIA_CART2][2] - 0x4000;
        }
        return;
    }

    // Otherwise see if we are able to write into the SRAM
    if (PAC_SRAM_in_view && (address >= 0x4000) && (address < 0x5FFE))
    {
        SRAM_Memory[address & 0x1FFF] = value;
        sram_show_status = 5;
    }
}

// --------------------------------------------------------------------
// Handle writes to the Disk Controller (FDC) memory mapped registers.
// The fdc_write() handler will route the request to the appropriate 
// controller: either the WD-2973 or the Toshiba TC controller.
// --------------------------------------------------------------------
void __attribute__((noinline)) HandleDiskControl(u16 address, u8 value)
{
    // For the TC Toshiba FDC driver...
    if (myConfig.fdc_driver == 1)
    {
        fdc_write(address & 0x0F, value);
        
        if (address == 0x7FF0)
        {
            dos22_bank = (value & 3);
            u8 *disk_rom = TURBO_DOS23 + (dos22_bank * 0x4000);
            MemoryMap[2] = disk_rom + 0x0000 - 0x4000;
            MemoryMap[3] = disk_rom + 0x2000 - 0x6000;
        }
    }
    else // For the WD-2973 FDC driver...
    {
        if (address <= 0x7FFB) fdc_write(address & 3, value);   // Command/data
        if (address == 0x7FFC) fdc_setSide((value & 1) ? 1:0);  // Side: [xxxxxxxS]
        if (address == 0x7FFD) fdc_setDrive((value & 3));       // Drive: [xxxxxxDD]
    }
}

typedef void (*MapperWriteHandler)(u8 media_id, u16 address, u8 value);

static MapperWriteHandler const mapperWriteHandlers[END_MAPPERS] __attribute__((section(".dtcm"))) =
{
    [KON8]              = HandleKonami8,
    [ASC8]              = HandleAscii8,
    [SCC8]              = HandleKonamiSCC8,
    [ASC16]             = HandleAscii16,
    [ASC16X]            = HandleAscii16X,
    [NEO8]              = HandleNeo8,
    [NEO16]             = HandleNeo16,
    [PAC_SRAM]          = Handle_PAC_SRAM,
};

// ---------------------------------------------------------------------
// Write memory handles both normal writes and bankswitched since
// write is much less common than reads... All of the MSX cart mappers
// are vectored here so we can handle all the bank switching, etc.
// ---------------------------------------------------------------------
ITCM_CODE void cpu_writemem16(u16 address, u8 value)
{
    if ((special_memory_access & (SPEC_MEM_SUBSLOT0_ACTIVE | SPEC_MEM_SUBSLOT3_ACTIVE)) && (address == 0xFFFF)) // Subslot check... only when page 3 is mapped to an expanded slot
    {
        if (special_memory_access & SPEC_MEM_SUBSLOT0_ACTIVE) msx_subslot0 = value;
        else msx_subslot3 = value;
        cpu_writeport_msx(0xA8, Port_PPI_A); // Enable the new map...
    }
    else if (bRAMInPage[address >> 14]) // RAM Exists in this slot... write it.
    {
        MemoryMap[address>>13][address]=value;
    }
    else if ((special_memory_access & SPEC_MEM_DISK_CONTROLLER) && (address >= 0x7FF0) && (address <= 0x7FFF)) // Disk controller memory map... (including Turbo-R DOS 2.3 if enabled)
    {
        HandleDiskControl(address, value);
    }
    else if ((special_memory_access & SPEC_MEM_EXTDOS22) && (address >= 0x7FFE) && (address <= 0x7FFF)) // External MSX-DOS 2.2 memory map...
    {
        HandleMSXDOS2(address, value);
    }
    else if (bCartInPage[address>>14]) // Is there a Cartridge mapped into the write address area?
    {
        u8 media_id = bCartInPage[address>>14] - 1;     // Is this MEDIA_CART1 or MEDIA_CART2?
        u8 mapper_type = MyMedia[media_id].mapperType;  // And what type of CART is it?

        // -----------------------------------------------------------------------
        // Each cartridge mapper is different - call into the appropriate handler.
        // A table-lookup here keeps this execution path relatively deterministic
        // no matter how many mappers we have.
        // -----------------------------------------------------------------------
        if (mapperWriteHandlers[mapper_type] != NULL)
        {
            mapperWriteHandlers[mapper_type](media_id, address, value);
        }
    }
}

// -----------------------------------------------------------------
// Reset a few key variables needed for proper Z80 interface use...
// -----------------------------------------------------------------
void Z80_Interface_Reset(void)
{
    CPU.CycleDeficit      = 0;
    PAC_SRAM_in_view      = 0;
    FMPAC_Key1            = 0;
}

// -----------------------------------------------------------------
// Trap and report illegal opcodes to the VroomDS debugger...
// -----------------------------------------------------------------
void Trap_Bad_Ops(char *prefix, byte I, word W)
{
    if (myGlobalConfig.debugger)
    {
        char tmp[32];
        sprintf(tmp, "ILLOP: %s %02X %04X", prefix, I, W);
        DSPrint(0,0,6, tmp);
    }
}

// End of file
