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
#include <nds.h>

#include <string.h>

#include "Vroom-DS.h"
#include "fdc_toshiba.h"
#include "MSX_generic.h"

#define TC_PHASE_IDLE       0
#define TC_PHASE_COMMAND    1
#define TC_PHASE_READ       2
#define TC_PHASE_WRITE      3
#define TC_PHASE_FORMAT     4
#define TC_PHASE_RESULT     5

#define TC_ST0_ABNORMAL     0x40
#define TC_ST0_INVALID      0x80
#define TC_ST1_NO_DATA      0x04
#define TC_ST3_TRACK0       0x10
#define TC_ST3_READY        0x20

// This models the TC8566AF command/FIFO interface using sector-addressable DSK
// images. Unlike openMSX's raw-track model, it does not emulate rotation, CRCs,
// deleted-data marks, or byte-by-byte DRQ timing.
struct ToshibaFDC_t          ToshibaFDC;
struct ToshibaFDCGeometry_t  ToshibaGeom;

static u8 fdc_toshiba_next_sector_missing;
static u8 fdc_toshiba_seek_in_progress;
static u8 fdc_toshiba_seek_drive;
static u8 fdc_toshiba_seek_target;
static u8 fdc_toshiba_step_rate;
static u8 fdc_toshiba_transfer_physical_head;

static u8 fdc_toshiba_main_status(void);
static void fdc_toshiba_execute_command(void);
static void fdc_toshiba_finish_transfer(void);

static u8 fdc_toshiba_size_code(void)
{
    u32 size = 128;
    for (u8 code = 0; code <= 7; code++, size <<= 1)
    {
        if (size == ToshibaGeom.sectorSize) return code;
    }
    return 0;
}

static u8 *fdc_toshiba_disk_ptr(u8 drive)
{
    switch (drive)
    {
        case 0: return ToshibaGeom.disk0;
        case 1: return ToshibaGeom.disk1;
        case 2: return ToshibaGeom.disk2;
        default: return NULL;
    }
}

static u32 fdc_toshiba_track_size(void)
{
    return (u32)ToshibaGeom.sectorSize * ToshibaGeom.sectors;
}

static u8 *fdc_toshiba_sector_ptr(u8 drive, u8 cylinder, u8 side, u8 sector)
{
    if (drive >= ToshibaGeom.drives || cylinder >= ToshibaGeom.tracks || side >= ToshibaGeom.sides ||
        sector < ToshibaGeom.startSector || sector >= (u16)ToshibaGeom.startSector + ToshibaGeom.sectors)
    {
        return NULL;
    }

    u8 *disk = fdc_toshiba_disk_ptr(drive);
    if (!disk) return NULL;

    u32 track_size = fdc_toshiba_track_size();
    u32 track_index = ((u32)ToshibaGeom.sides * cylinder) + side;
    u32 sector_index = (u32)(sector - ToshibaGeom.startSector);
    return disk + track_index * track_size + sector_index * ToshibaGeom.sectorSize;
}

// The image is linearized by cylinder, then side, then sector.
void fdc_toshiba_buffer_track(void)
{
    u32 track_size = fdc_toshiba_track_size();
    u8 *disk = fdc_toshiba_disk_ptr(ToshibaFDC.drive);

    memset(ToshibaFDC.track_buffer, 0, sizeof(ToshibaFDC.track_buffer));
    if (disk && ToshibaFDC.track < ToshibaGeom.tracks && ToshibaFDC.side < ToshibaGeom.sides &&
        track_size <= sizeof(ToshibaFDC.track_buffer))
    {
        u32 track_index = ((u32)ToshibaGeom.sides * ToshibaFDC.track) + ToshibaFDC.side;
        memcpy(ToshibaFDC.track_buffer, disk + track_index * track_size, track_size);
    }

    if (ToshibaFDC.drive < sizeof(ToshibaFDC.any_track_dirty))
        ToshibaFDC.any_track_dirty[ToshibaFDC.drive] = 0;
}

// Commit a changed track buffer to the disk image in memory.
void fdc_toshiba_flush_track(void)
{
    if (ToshibaFDC.drive >= sizeof(ToshibaFDC.any_track_dirty) ||
        !ToshibaFDC.any_track_dirty[ToshibaFDC.drive])
    {
        return;
    }

    u32 track_size = fdc_toshiba_track_size();
    u8 *disk = fdc_toshiba_disk_ptr(ToshibaFDC.drive);
    if (disk && ToshibaFDC.track < ToshibaGeom.tracks && ToshibaFDC.side < ToshibaGeom.sides &&
        track_size <= sizeof(ToshibaFDC.track_buffer))
    {
        u32 track_index = ((u32)ToshibaGeom.sides * ToshibaFDC.track) + ToshibaFDC.side;
        memcpy(disk + track_index * track_size, ToshibaFDC.track_buffer, track_size);
        if (ToshibaFDC.side < 2 && ToshibaFDC.track < TOSHIBA_MAX_TRACKS)
            ToshibaFDC.tracks_to_write[ToshibaFDC.side][ToshibaFDC.track] = 1;
    }

    ToshibaFDC.any_track_dirty[ToshibaFDC.drive] = 0;
}

void fdc_toshiba_persist_disk(void)
{
}

static u8 fdc_toshiba_main_status(void)
{
    u8 status;

    // RQM/DIO/CB reflect the current FIFO direction and controller phase.
    // Byte-transfer RQM is immediate here; the real controller paces it.
    switch (ToshibaFDC.tc_phase)
    {
        case TC_PHASE_COMMAND: status = 0x90; break;
        case TC_PHASE_READ:    status = 0xD0; break;
        case TC_PHASE_WRITE:
        case TC_PHASE_FORMAT:  status = 0x90; break;
        case TC_PHASE_RESULT:  status = 0xD0; break;
        default:               status = 0x80; break;
    }

    if (ToshibaFDC.tc_phase == TC_PHASE_READ || ToshibaFDC.tc_phase == TC_PHASE_WRITE ||
        ToshibaFDC.tc_phase == TC_PHASE_FORMAT)
    {
        if (ToshibaFDC.tc_ndma) status |= 0x20;
    }

    if (fdc_toshiba_seek_in_progress && ToshibaFDC.busy_countdown && fdc_toshiba_seek_drive < 4)
        status |= (u8)(1U << fdc_toshiba_seek_drive);

    if (ToshibaFDC.tc_rqm_delay)
        status &= ~0x80;

    ToshibaFDC.status = status;
    return status;
}

static void fdc_toshiba_set_result(const u8 *result, u8 count)
{
    if (count > sizeof(ToshibaFDC.tc_result)) count = sizeof(ToshibaFDC.tc_result);
    memcpy(ToshibaFDC.tc_result, result, count);
    ToshibaFDC.tc_result_count = count;
    ToshibaFDC.tc_result_index = 0;
    ToshibaFDC.tc_phase = TC_PHASE_RESULT;
    ToshibaFDC.int_req = 0;
    fdc_toshiba_main_status();
}

static u8 fdc_toshiba_status0(u8 abnormal)
{
    return (ToshibaFDC.drive & 3) | ((ToshibaFDC.side & 1) << 2) |
           (abnormal ? TC_ST0_ABNORMAL : 0);
}

static void fdc_toshiba_set_transfer_result(u8 st0, u8 st1, u8 st2)
{
    u8 result[7];
    result[0] = st0;
    result[1] = st1;
    result[2] = st2;
    result[3] = ToshibaFDC.tc_transfer_cylinder;
    result[4] = ToshibaFDC.tc_transfer_head;
    result[5] = ToshibaFDC.tc_transfer_sector;
    result[6] = ToshibaFDC.tc_parameters[4];
    fdc_toshiba_set_result(result, sizeof(result));
}

static u8 fdc_toshiba_finish_missing_sector(void)
{
    if (!fdc_toshiba_next_sector_missing ||
        (ToshibaFDC.tc_phase != TC_PHASE_READ && ToshibaFDC.tc_phase != TC_PHASE_WRITE))
    {
        return 0;
    }

    fdc_toshiba_next_sector_missing = 0;
    if (ToshibaFDC.tc_phase == TC_PHASE_WRITE)
        fdc_toshiba_flush_track();
    fdc_toshiba_set_transfer_result(fdc_toshiba_status0(1), TC_ST1_NO_DATA, 0);
    return 1;
}

static void fdc_toshiba_set_invalid_command(void)
{
    u8 result = TC_ST0_INVALID;
    fdc_toshiba_set_result(&result, 1);
}

static void fdc_toshiba_begin_data_transfer(u8 write)
{
    u8 *sector;
    u8 unit_head = ToshibaFDC.tc_parameters[0];

    ToshibaFDC.drive = unit_head & 3;
    fdc_toshiba_transfer_physical_head = (unit_head >> 2) & 1;
    ToshibaFDC.side = fdc_toshiba_transfer_physical_head;
    ToshibaFDC.tc_transfer_cylinder = ToshibaFDC.tc_parameters[1];
    ToshibaFDC.tc_transfer_head = ToshibaFDC.tc_parameters[2] & 1;
    ToshibaFDC.tc_transfer_sector = ToshibaFDC.tc_parameters[3];
    ToshibaFDC.tc_transfer_eot = ToshibaFDC.tc_parameters[5];
    fdc_toshiba_next_sector_missing = 0;

    ToshibaFDC.track = ToshibaFDC.tc_transfer_cylinder;
    ToshibaFDC.sector = ToshibaFDC.tc_transfer_sector;
    ToshibaFDC.tc_status0 = fdc_toshiba_status0(0);
    ToshibaFDC.tc_status1 = 0;
    ToshibaFDC.tc_status2 = 0;

    sector = fdc_toshiba_sector_ptr(ToshibaFDC.drive, ToshibaFDC.track, ToshibaFDC.side, ToshibaFDC.sector);
    if (!sector || !ToshibaGeom.sectorSize ||
        ToshibaFDC.tc_transfer_eot < ToshibaFDC.tc_transfer_sector ||
        fdc_toshiba_track_size() > sizeof(ToshibaFDC.track_buffer))
    {
        fdc_toshiba_set_transfer_result((u8)(fdc_toshiba_status0(1)), TC_ST1_NO_DATA, 0);
        return;
    }

    fdc_toshiba_buffer_track();
    ToshibaFDC.track_buffer_idx =
        (u16)(ToshibaFDC.tc_transfer_sector - ToshibaGeom.startSector) * ToshibaGeom.sectorSize;
    // DTL is not the sector length for normal data commands; openMSX derives
    // the length from the sector ID. The flat image supplies it via geometry.
    ToshibaFDC.tc_transfer_size = ToshibaGeom.sectorSize;
    ToshibaFDC.tc_transfer_offset = 0;
    ToshibaFDC.tc_phase = write ? TC_PHASE_WRITE : TC_PHASE_READ;
    ToshibaFDC.wait_for_read = write ? 2 : 0;
    ToshibaFDC.wait_for_write = write ? 0 : 2;
    fdc_toshiba_main_status();

    if (io_show_status == 0)
        io_show_status = write ? 8 : 7;
}

static void fdc_toshiba_start_next_sector(void)
{
    if (ToshibaFDC.tc_transfer_sector < ToshibaFDC.tc_transfer_eot)
    {
        ToshibaFDC.tc_transfer_sector++;
    }
    else if ((ToshibaFDC.command & 0x80) && ToshibaFDC.tc_transfer_head == 0 &&
             ToshibaGeom.sides > 1)
    {
        if (ToshibaFDC.tc_phase == TC_PHASE_WRITE) fdc_toshiba_flush_track();
        ToshibaFDC.tc_transfer_head = 1;
        fdc_toshiba_transfer_physical_head ^= 1;
        ToshibaFDC.tc_transfer_sector = ToshibaGeom.startSector;
        ToshibaFDC.side = fdc_toshiba_transfer_physical_head;
        ToshibaFDC.track = ToshibaFDC.tc_transfer_cylinder;
        fdc_toshiba_buffer_track();
    }
    else
    {
        if (ToshibaFDC.tc_phase == TC_PHASE_WRITE) fdc_toshiba_flush_track();
        fdc_toshiba_set_transfer_result(fdc_toshiba_status0(0), ToshibaFDC.tc_status1, ToshibaFDC.tc_status2);
        return;
    }

    ToshibaFDC.side = fdc_toshiba_transfer_physical_head;
    ToshibaFDC.sector = ToshibaFDC.tc_transfer_sector;
    ToshibaFDC.track_buffer_idx =
        (u16)(ToshibaFDC.tc_transfer_sector - ToshibaGeom.startSector) * ToshibaGeom.sectorSize;
    ToshibaFDC.tc_transfer_offset = 0;

    if (!fdc_toshiba_sector_ptr(ToshibaFDC.drive, ToshibaFDC.track, ToshibaFDC.side, ToshibaFDC.sector))
    {
        fdc_toshiba_next_sector_missing = 1;
    }
}

static void fdc_toshiba_finish_transfer(void)
{
    if (ToshibaFDC.tc_phase == TC_PHASE_WRITE)
    {
        if (ToshibaFDC.drive < sizeof(ToshibaFDC.any_track_dirty))
            ToshibaFDC.any_track_dirty[ToshibaFDC.drive] = 1;
    }

    ToshibaFDC.tc_transfer_offset = 0;
    fdc_toshiba_start_next_sector();
}

static u8 fdc_toshiba_read_fifo(void)
{
    fdc_toshiba_finish_missing_sector();

    if (ToshibaFDC.tc_phase == TC_PHASE_RESULT)
    {
        u8 value = ToshibaFDC.tc_result[ToshibaFDC.tc_result_index++];
        ToshibaFDC.data = value;
        if (ToshibaFDC.tc_result_index >= ToshibaFDC.tc_result_count)
        {
            ToshibaFDC.tc_phase = TC_PHASE_IDLE;
            ToshibaFDC.tc_result_count = 0;
            ToshibaFDC.tc_result_index = 0;
            ToshibaFDC.wait_for_read = 2;
            ToshibaFDC.wait_for_write = 2;
        }
        fdc_toshiba_main_status();
        return value;
    }

    if (ToshibaFDC.tc_phase == TC_PHASE_READ)
    {
        u32 buffer_index = (u32)ToshibaFDC.track_buffer_idx + ToshibaFDC.tc_transfer_offset;
        if (buffer_index >= sizeof(ToshibaFDC.track_buffer))
        {
            fdc_toshiba_set_transfer_result(fdc_toshiba_status0(1), TC_ST1_NO_DATA, 0);
            return 0xFF;
        }

        u8 value = ToshibaFDC.track_buffer[buffer_index];
        ToshibaFDC.data = value;
        if (++ToshibaFDC.tc_transfer_offset >= ToshibaFDC.tc_transfer_size)
            fdc_toshiba_finish_transfer();
        fdc_toshiba_main_status();
        return value;
    }

    return 0xFF;
}

static void fdc_toshiba_complete_format(void)
{
    u8 count = ToshibaFDC.tc_parameters[2];
    u8 fill = ToshibaFDC.tc_parameters[4];
    u8 n = ToshibaFDC.tc_parameters[1];
    u32 format_size = n <= 7 ? ((u32)128 << n) : 0;
    u8 *ids = ToshibaFDC.track_buffer;
    ToshibaFDC.tc_parameters[4] = n;

    if (!format_size || format_size != ToshibaGeom.sectorSize ||
        ToshibaFDC.drive >= ToshibaGeom.drives || ToshibaFDC.track >= ToshibaGeom.tracks ||
        ToshibaFDC.side >= ToshibaGeom.sides)
    {
        fdc_toshiba_set_transfer_result(fdc_toshiba_status0(1), TC_ST1_NO_DATA, 0);
        return;
    }

    for (u8 i = 0; i < count; i++)
    {
        u8 *id = ids + (u32)i * 4;
        u8 *sector = fdc_toshiba_sector_ptr(ToshibaFDC.drive, id[0], id[1] & 1, id[2]);
        if (!sector || id[0] != ToshibaFDC.track || (id[1] & 1) != ToshibaFDC.side ||
            id[3] != n)
        {
            fdc_toshiba_set_transfer_result(fdc_toshiba_status0(1), TC_ST1_NO_DATA, 0);
            return;
        }
    }

    for (u8 i = 0; i < count; i++)
    {
        u8 *id = ids + (u32)i * 4;
        u8 *sector = fdc_toshiba_sector_ptr(ToshibaFDC.drive, id[0], id[1] & 1, id[2]);
        memset(sector, fill, ToshibaGeom.sectorSize);
    }

    if (ToshibaFDC.side < 2 && ToshibaFDC.track < TOSHIBA_MAX_TRACKS)
        ToshibaFDC.tracks_to_write[ToshibaFDC.side][ToshibaFDC.track] = 1;

    ToshibaFDC.tc_transfer_cylinder = ToshibaFDC.track;
    ToshibaFDC.tc_transfer_head = ToshibaFDC.side;
    ToshibaFDC.tc_transfer_sector = count ? ids[(u32)(count - 1) * 4 + 2] : ToshibaGeom.startSector;
    ToshibaFDC.tc_parameters[4] = n;
    fdc_toshiba_set_transfer_result(fdc_toshiba_status0(0), 0, 0);
}

static void fdc_toshiba_execute_command(void)
{
    u8 operation = ToshibaFDC.command & 0x1F;

    switch (operation)
    {
        case 0x03: // Specify
            ToshibaFDC.tc_ndma = ToshibaFDC.tc_parameters[1] & 1;
            fdc_toshiba_step_rate = ToshibaFDC.tc_parameters[0] >> 4;
            ToshibaFDC.tc_phase = TC_PHASE_IDLE;
            fdc_toshiba_main_status();
            break;

        case 0x04: // Sense drive status
        {
            u8 unit_head = ToshibaFDC.tc_parameters[0];
            u8 drive = unit_head & 3;
            u8 side = (unit_head >> 2) & 1;
            u8 status = (u8)(drive | (side << 2));
            if (drive < ToshibaGeom.drives && fdc_toshiba_disk_ptr(drive))
            {
                if (ToshibaGeom.sides > 1) status |= 0x08;
                if (ToshibaFDC.tc_current_track[drive] == 0) status |= TC_ST3_TRACK0;
                status |= TC_ST3_READY;
            }
            u8 result = status;
            fdc_toshiba_set_result(&result, 1);
            break;
        }

        case 0x05: // Write data
        case 0x09: // Write deleted data
        case 0x06: // Read data
        case 0x0C: // Read deleted data
            fdc_toshiba_begin_data_transfer(operation == 0x05 || operation == 0x09);
            break;

        case 0x07: // Recalibrate
        case 0x0F: // Seek
        {
            u8 drive = ToshibaFDC.tc_parameters[0] & 3;
            u8 current_track = drive < 2 ? ToshibaFDC.tc_current_track[drive] : 0;
            u8 target_track = operation == 0x07 ? 0 : ToshibaFDC.tc_parameters[1];
            ToshibaFDC.drive = drive;
            ToshibaFDC.side = (ToshibaFDC.tc_parameters[0] >> 2) & 1;
            fdc_toshiba_seek_drive = drive;
            fdc_toshiba_seek_target = target_track;
            ToshibaFDC.tc_pending_st0 = (u8)(drive | ((ToshibaFDC.side & 1) << 2) | 0x20);
            ToshibaFDC.tc_pending_track = target_track;
            ToshibaFDC.tc_pending_interrupt = 0;
            ToshibaFDC.int_req = 0;
            fdc_toshiba_seek_in_progress = 1;
            u8 steps = current_track > target_track
                           ? current_track - target_track
                           : target_track - current_track;
            if (operation == 0x07 && current_track == 0)
                steps = 0;
            u8 step_time_ms = (u8)(16 - fdc_toshiba_step_rate);
            if (steps)
                ToshibaFDC.busy_countdown = (u32)steps * step_time_ms * 16;
            else
            {
                ToshibaFDC.busy_countdown = 0;
                fdc_toshiba_seek_in_progress = 0;
                if (drive < 2)
                    ToshibaFDC.tc_current_track[drive] = target_track;
                ToshibaFDC.tc_pending_interrupt = 1;
                ToshibaFDC.int_req = 0x80;
            }
            ToshibaFDC.tc_phase = TC_PHASE_IDLE;
            fdc_toshiba_main_status();
            break;
        }

        case 0x08: // Sense interrupt status
        {
            u8 result[2];
            if (ToshibaFDC.tc_pending_interrupt)
            {
                result[0] = ToshibaFDC.tc_pending_st0;
                result[1] = ToshibaFDC.tc_pending_track;
                ToshibaFDC.tc_pending_interrupt = 0;
                ToshibaFDC.tc_pending_st0 = 0;
                ToshibaFDC.int_req = 0;
            }
            else
            {
                // SENSE INTERRUPT STATUS still returns ST0 and the current
                // cylinder when there is no pending seek completion.
                result[0] = 0;
                result[1] = ToshibaFDC.drive < 2
                                ? ToshibaFDC.tc_current_track[ToshibaFDC.drive]
                                : 0;
            }
            fdc_toshiba_set_result(result, sizeof(result));
            break;
        }

        case 0x0A: // Read ID
        {
            // A flat DSK has no rotational ID stream; report its first sector.
            u8 unit_head = ToshibaFDC.tc_parameters[0];
            u8 drive = unit_head & 3;
            ToshibaFDC.drive = drive;
            ToshibaFDC.side = (unit_head >> 2) & 1;
            ToshibaFDC.track = drive < 2 ? ToshibaFDC.tc_current_track[drive] : 0;
            ToshibaFDC.tc_transfer_cylinder = ToshibaFDC.track;
            ToshibaFDC.tc_transfer_head = ToshibaFDC.side;
            ToshibaFDC.tc_transfer_sector = ToshibaGeom.startSector;
            ToshibaFDC.tc_parameters[4] = fdc_toshiba_size_code();
            if (!fdc_toshiba_sector_ptr(ToshibaFDC.drive, ToshibaFDC.track, ToshibaFDC.side, ToshibaGeom.startSector))
                fdc_toshiba_set_transfer_result(fdc_toshiba_status0(1), TC_ST1_NO_DATA, 0);
            else
                fdc_toshiba_set_transfer_result(fdc_toshiba_status0(0), 0, 0);
            break;
        }

        case 0x0D: // Format track
        {
            u8 unit_head = ToshibaFDC.tc_parameters[0];
            ToshibaFDC.drive = unit_head & 3;
            ToshibaFDC.side = (unit_head >> 2) & 1;
            ToshibaFDC.track = ToshibaFDC.drive < 2 ? ToshibaFDC.tc_current_track[ToshibaFDC.drive] : 0;
            ToshibaFDC.tc_transfer_cylinder = ToshibaFDC.track;
            ToshibaFDC.tc_transfer_head = ToshibaFDC.side;
            ToshibaFDC.tc_transfer_sector = ToshibaGeom.startSector;
            ToshibaFDC.write_track_byte_counter = 0;
            if (ToshibaFDC.tc_parameters[2] == 0 ||
                (u32)ToshibaFDC.tc_parameters[2] * 4 > sizeof(ToshibaFDC.track_buffer))
            {
                ToshibaFDC.tc_parameters[4] = ToshibaFDC.tc_parameters[1];
                fdc_toshiba_set_transfer_result(fdc_toshiba_status0(1), TC_ST1_NO_DATA, 0);
            }
            else
            {
                ToshibaFDC.tc_phase = TC_PHASE_FORMAT;
                fdc_toshiba_main_status();
            }
            break;
        }

        default:
            fdc_toshiba_set_invalid_command();
            break;
    }
}

static u8 fdc_toshiba_command_parameter_count(u8 command)
{
    switch (command & 0x1F)
    {
        case 0x03: return 2;
        case 0x04: return 1;
        case 0x05:
        case 0x06:
        case 0x09:
        case 0x0C: return 8;
        case 0x07: return 1;
        case 0x08: return 0;
        case 0x0A: return 1;
        case 0x0D: return 5;
        case 0x0F: return 2;
        default:   return 0;
    }
}

static void fdc_toshiba_start_command(u8 command)
{
    ToshibaFDC.command = command;
    ToshibaFDC.tc_command_count = fdc_toshiba_command_parameter_count(command);
    ToshibaFDC.tc_command_index = 0;
    if ((command & 0x1F) == 0x08)
    {
        fdc_toshiba_execute_command();
    }
    else if (ToshibaFDC.tc_command_count)
    {
        ToshibaFDC.tc_phase = TC_PHASE_COMMAND;
        fdc_toshiba_main_status();
    }
    else
    {
        fdc_toshiba_set_invalid_command();
    }
}

static void fdc_toshiba_write_fifo(u8 value)
{
    ToshibaFDC.data = value;
    fdc_toshiba_finish_missing_sector();
    if (ToshibaFDC.tc_phase == TC_PHASE_IDLE)
    {
        fdc_toshiba_start_command(value);
        return;
    }

    if (ToshibaFDC.tc_phase == TC_PHASE_COMMAND)
    {
        if (ToshibaFDC.tc_command_index < sizeof(ToshibaFDC.tc_parameters))
            ToshibaFDC.tc_parameters[ToshibaFDC.tc_command_index] = value;
        ToshibaFDC.tc_command_index++;
        if (ToshibaFDC.tc_command_index >= ToshibaFDC.tc_command_count)
            fdc_toshiba_execute_command();
        else
            fdc_toshiba_main_status();
        return;
    }

    if (ToshibaFDC.tc_phase == TC_PHASE_WRITE)
    {
        u32 buffer_index = (u32)ToshibaFDC.track_buffer_idx + ToshibaFDC.tc_transfer_offset;
        if (buffer_index >= sizeof(ToshibaFDC.track_buffer))
        {
            fdc_toshiba_set_transfer_result(fdc_toshiba_status0(1), TC_ST1_NO_DATA, 0);
            return;
        }

        ToshibaFDC.track_buffer[buffer_index] = value;
        if (++ToshibaFDC.tc_transfer_offset >= ToshibaFDC.tc_transfer_size)
            fdc_toshiba_finish_transfer();
        fdc_toshiba_main_status();
        return;
    }

    if (ToshibaFDC.tc_phase == TC_PHASE_FORMAT)
    {
        u32 format_bytes = (u32)ToshibaFDC.tc_parameters[2] * 4;
        if (ToshibaFDC.write_track_byte_counter < sizeof(ToshibaFDC.track_buffer))
            ToshibaFDC.track_buffer[ToshibaFDC.write_track_byte_counter] = value;
        ToshibaFDC.write_track_byte_counter++;
        if (ToshibaFDC.write_track_byte_counter >= format_bytes)
            fdc_toshiba_complete_format();
        fdc_toshiba_main_status();
    }
}

void fdc_toshiba_loop(void)
{
    // Approximate seek latency in scanline ticks; this lands on the target
    // track at completion rather than emulating individual physical steps.
    if (ToshibaFDC.busy_countdown && --ToshibaFDC.busy_countdown == 0 &&
        fdc_toshiba_seek_in_progress)
    {
        if (fdc_toshiba_seek_drive < 2)
            ToshibaFDC.tc_current_track[fdc_toshiba_seek_drive] = fdc_toshiba_seek_target;
        if (fdc_toshiba_seek_drive == ToshibaFDC.drive)
            ToshibaFDC.track = fdc_toshiba_seek_target;
        fdc_toshiba_seek_in_progress = 0;
        ToshibaFDC.tc_pending_interrupt = 1;
        ToshibaFDC.int_req = 0x80;
        fdc_toshiba_main_status();
    }
}

u8 fdc_toshiba_read(u8 addr)
{
    switch (addr)
    {
        case 0: return ToshibaFDC.tc_rom_bank;
        case 1: return 0x03;    // No disk-change flags are modeled
        case 4:
        {
            fdc_toshiba_finish_missing_sector();
            return fdc_toshiba_main_status();
        }
        case 5: return fdc_toshiba_read_fifo();
        case 12:
        case 13: return 0xFC;
        case 15: return 0x3F;
        default: return 0xFF;
    }
}

void fdc_toshiba_write(u8 addr, u8 data)
{
    switch (addr)
    {
        case 0: // Disk-ROM bank register
            ToshibaFDC.tc_rom_bank = data & 0x03;
            break;

        case 2: // Digital output register
            ToshibaFDC.tc_dor = data;
            ToshibaFDC.motor = (data >> 4) & 3;
            fdc_toshiba_setDrive(data & 3);
            fdc_toshiba_main_status();
            break;

        case 3: // Controller configuration register
            ToshibaFDC.tc_cr1 = data;
            if ((data & 0x01) &&
                (ToshibaFDC.tc_phase == TC_PHASE_READ ||
                 ToshibaFDC.tc_phase == TC_PHASE_WRITE ||
                 ToshibaFDC.tc_phase == TC_PHASE_FORMAT))
            {
                fdc_toshiba_next_sector_missing = 0;
                if (ToshibaFDC.tc_phase == TC_PHASE_FORMAT)
                    ToshibaFDC.tc_parameters[4] = ToshibaFDC.tc_parameters[1];
                if (ToshibaFDC.tc_phase == TC_PHASE_WRITE)
                {
                    if (ToshibaFDC.drive < sizeof(ToshibaFDC.any_track_dirty))
                        ToshibaFDC.any_track_dirty[ToshibaFDC.drive] = 1;
                    fdc_toshiba_flush_track();
                }
                fdc_toshiba_set_transfer_result(fdc_toshiba_status0(0), 0, 0);
            }
            break;

        case 5: // Command/result/data FIFO
            fdc_toshiba_write_fifo(data);
            break;

        default:
            break;
    }
}

void fdc_toshiba_setDrive(u8 drive)
{
    ToshibaFDC.drive = drive;
}

void fdc_toshiba_setSide(u8 side)
{
    ToshibaFDC.side = side & 1;
}

void fdc_toshiba_reset(u8 full_reset)
{
    if (full_reset)
        memset(&ToshibaFDC, 0, sizeof(ToshibaFDC));

    ToshibaFDC.status = 0x80;
    ToshibaFDC.command = 0;
    ToshibaFDC.commandType = 0;
    ToshibaFDC.tc_phase = TC_PHASE_IDLE;
    ToshibaFDC.tc_command_count = 0;
    ToshibaFDC.tc_command_index = 0;
    ToshibaFDC.tc_result_count = 0;
    ToshibaFDC.tc_result_index = 0;
    ToshibaFDC.tc_pending_interrupt = 0;
    fdc_toshiba_seek_in_progress = 0;
    fdc_toshiba_seek_drive = 0;
    fdc_toshiba_seek_target = 0;
    fdc_toshiba_step_rate = 0;
    fdc_toshiba_transfer_physical_head = 0;
    fdc_toshiba_next_sector_missing = 0;
    ToshibaFDC.tc_dor = 0x0C;
    ToshibaFDC.tc_cr1 = 0;
    ToshibaFDC.tc_ndma = 1;
    ToshibaFDC.int_req = 0;
    ToshibaFDC.tc_rqm_delay = 0;
    ToshibaFDC.wait_for_read = 2;
    ToshibaFDC.wait_for_write = 2;
    ToshibaFDC.read_timeout = 0;
    ToshibaFDC.busy_countdown = 0;
    fdc_toshiba_main_status();
}

void fdc_toshiba_init(u8 drives, u8 sides, u8 tracks, u8 sectors, u16 sectorSize,
              u8 startSector, u8 *diskBuffer0, u8 *diskBuffer1)
{
    ToshibaGeom.drives = drives;
    ToshibaGeom.sides = sides;
    ToshibaGeom.tracks = tracks;
    ToshibaGeom.sectors = sectors;
    ToshibaGeom.sectorSize = sectorSize;
    ToshibaGeom.disk0 = diskBuffer0;
    ToshibaGeom.disk1 = diskBuffer1;
    ToshibaGeom.disk2 = NULL;
    ToshibaGeom.startSector = startSector;
}
