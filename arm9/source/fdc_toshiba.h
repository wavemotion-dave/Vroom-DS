// =====================================================================================
// Copyright (c) 2026 Dave Bernazzani (wavemotion-dave)
//
// Copying and distribution of this emulator, its source code and associated
// readme files, with or without modification, are permitted in any medium without
// royalty provided this copyright notice is used and wavemotion-dave and
// Marat Fayzullin (fMSX core) are thanked profusely.
//
// The Vroom-DS emulator is offered as-is, without any warranty. Please see readme.md
// =====================================================================================

#ifndef _FDC_TOSHIBA_H
#define _FDC_TOSHIBA_H

#include <nds.h>

#define TOSHIBA_MAX_TRACKS 80

// Toshiba TC8566AF state. The tc_* members hold the 765-style command,
// transfer, and result phases; the earlier fields are shared with emulator
// diagnostics and disk-image buffering.
struct ToshibaFDC_t
{
    u8  status;
    u8  command;
    u8  track;
    u8  sector;
    u8  data;
    u8  drive;
    u8  side;
    u8  motor;                 // Four-bit TC8566AF motor-enable mask (CR0 D7-D4)
    u8  wait_for_read;
    u8  wait_for_write;
    u8  commandType;
    u8  write_track_allowed;
    u8  stepDirection;
    u8  int_req;
    u8  read_timeout;
    u8  seekDestination;
    u8  any_track_dirty[2];
    u8  tracks_to_write[2][TOSHIBA_MAX_TRACKS];
    u8  track_buffer[10240];
    u16 track_buffer_idx;
    u16 track_buffer_end;
    u16 indexPulseCounter;
    u16 sector_byte_counter;
    u16 write_track_byte_counter;
    u32 busy_countdown;
    u8  tc_phase;
    u8  tc_command_count;
    u8  tc_command_index;
    u8  tc_result_count;
    u8  tc_result_index;
    u8  tc_parameters[8];  // Maximum command parameter list (read/write data)
    u8  tc_result[7];      // ST0-ST2 plus returned C/H/R/N
    u8  tc_status0;
    u8  tc_status1;
    u8  tc_status2;
    u8  tc_status3;
    u8  tc_current_track[2];
    u8  tc_pending_interrupt;
    u8  tc_pending_st0;
    u8  tc_pending_track;
    u8  tc_dor;
    u8  tc_cr1;
    u8  tc_ndma;
    u8  tc_rom_bank;
    u8  tc_rqm_delay;
    u8  tc_transfer_head;
    u8  tc_transfer_cylinder;
    u8  tc_transfer_sector;
    u16 tc_transfer_size;
    u8  tc_transfer_eot;
    u16 tc_transfer_offset;
};

struct ToshibaFDCGeometry_t
{
    u8  drives;
    u8  sides;
    u8  tracks;
    u8  sectors;
    u16 sectorSize;
    u8  startSector;
    u8 *disk0;
    u8 *disk1;
    u8 *disk2;
};

extern struct ToshibaFDC_t          ToshibaFDC;
extern struct ToshibaFDCGeometry_t  ToshibaGeom;

// Register addresses are offsets in the Toshiba 16-byte memory-mapped window.
// The bank register is at 0, DOR at 2, CR1 at 3, and MSR/FIFO at 4/5.
u8   fdc_toshiba_read(u8 addr);
void fdc_toshiba_write(u8 addr, u8 data);
void fdc_toshiba_setDrive(u8 drive);
void fdc_toshiba_setSide(u8 side);
void fdc_toshiba_reset(u8 full_reset);
void fdc_toshiba_init(u8 drives, u8 sides, u8 tracks, u8 sectors,
                      u16 sectorSize, u8 startSector,
                      u8 *diskBuffer0, u8 *diskBuffer1);
void fdc_toshiba_persist_disk(void);
void fdc_toshiba_loop(void);

#endif
