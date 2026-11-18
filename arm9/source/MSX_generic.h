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
#ifndef _MSX_GENERIC_H_
#define _MSX_GENERIC_H_

#include "Vroom-DS.h"
#include "msx_music.h"
#include "cpu/z80/Z80_interface.h"
#include "cpu/vdp9938/vdp9938.h"
#include "cpu/ay38910/AY38910.h"
#include "cpu/scc/SCC.h"

#define likely(x)   __builtin_expect(!!(x), 1)
#define unlikely(x) __builtin_expect(!!(x), 0)

#define MAX_CONFIGS                 512
#define CONFIG_VER                  0x0006

#define MSXROM                      0x01
#define DIRECTORY                   0x02

#define ID_SHM_CANCEL               0x00
#define ID_SHM_YES                  0x01
#define ID_SHM_NO                   0x02

#define DPAD_NORMAL                 0
#define DPAD_DIAGONALS              1
#define DPAD_MOUSE                  2
#define DPAD_SLIDE_N_GLIDE          3

#define OVL_FULLKBD                 0
#define OVL_ALPHAKBD                1

typedef struct {
  char szName[MAX_FILE_NAME_LEN+1];
  u8 uType;
  u32 uCrc;
} FI_MSX;

struct __attribute__((__packed__)) GlobalConfig_t
{
    u16 config_ver;
    u32 bios_checksums;
    char szLastRom[MAX_FILE_NAME_LEN+1];
    char szLastPath[MAX_FILE_NAME_LEN+1];
    char reserved1[MAX_FILE_NAME_LEN+1];
    char reserved2[MAX_FILE_NAME_LEN+1];
    u8  showFPS;
    u8  bDiskSounds;
    u8  global_3;
    u8  global_4;
    u8  global_5;
    u8  global_6;
    u8  global_7;
    u8  debugger;
    u32 config_checksum;
};

struct __attribute__((__packed__)) Config_t
{
    u32 game_crc;
    u8  keymap[12];
    u8  msxMapper;
    u8  autoFire;
    u8  maxSprites;
    u8  dpad;
    u8  yOffset;
    u8  cpuBoost;
    u8  fdc_driver;
    u8  scaleScreen;
    u8  frameSkip;
    u8  ymBalance;
    u8  overscan;
    u8  MIDI;
    u8  pcm;
    u8  fakeFMPAC;
    u8  splitRefresh;
    u8  reserved2;
    u8  reserved3;
    u8  reserved4;
    u8  reserved5;
    u8  reserved6;
};

#define PCM_SAMPLE_BUFFER_SIZE 4096
#define PCM_SAMPLE_BUFFER_MASK (PCM_SAMPLE_BUFFER_SIZE - 1)
#define PCM_SAMPLE_DELAY_FRAMES 8
#define PCM_SAMPLE_SILENCE_FRAMES 20
extern u8 pcm_last_a4;
extern u8 pcm_last_a5;
extern u8 pcm_samples[PCM_SAMPLE_BUFFER_SIZE];
extern volatile u16 pcm_sample_read;
extern volatile u16 pcm_sample_write;
extern volatile u8 pcm_sample_delay_frames;
extern volatile u8 pcm_sample_silence_frames;
extern volatile u8 pcm_sample_frame_active;
extern u8 pcm_sample_current;
extern s32 pcm_sample_filtered;

#define NORAM                       0xFF    // When reading IO that is unmapped... we just return 0xFF

#define FS_NONE                     0       // No Frame Skip
#define FS_LIGHT                    1       // Light Frame Skip
#define FS_HEAVY                    2       // Heavy Frame Skip

#define CPU_BOOST_SLOW              0       // A bit slower than normal
#define CPU_BOOST_MED               1       // Roughly normal
#define CPU_BOOST_FAST              2       // A bit faster than normal

#define FDC_WD                      0       // Older WD2793 driver
#define FDC_TOSH                    1       // MSX Turbo-R Toshiba Driver

#define YM_BAL_LOW                  0       // Low YM volume mix
#define YM_BAL_MED                  1       // Medium YM volume mix
#define YM_BAL_HIGH                 2       // High YM volume mix
#define YM_BAL_MAX                  3       // Max (normal) YM volume mix

#define VDP_9958                    4       // Must be returned from V9938 status reads...

extern struct Config_t       myConfig;
extern struct GlobalConfig_t myGlobalConfig;

extern u8 special_memory_access;                // 0x00 if no special access, otherwise one or more of the SPEC_MEM_xxx below

#define SPEC_MEM_SUBSLOT3_ACTIVE    0x01        // Allows read/write to special SubSlot register at 0xFFFF - Slot 3
#define SPEC_MEM_SUBSLOT0_ACTIVE    0x02        // Allows read/write to special SubSlot register at 0xFFFF - Slot 0
#define SPEC_MEM_EXTDOS22           0x04        // External DOS 2.2 mapped into view
#define SPEC_MEM_DISK_CONTROLLER    0x08        // Disk Controller - either internal (Turbo-R 64K integrated) or external (16K Standalone)
#define SPEC_MEM_SCC_ENABLED        0x10        // Konami SCC registers in view

extern u8 last_special_key;
extern u8 last_special_key_dampen;
extern u16 msx_init;
extern u16 msx_basic;
extern u8 skip_render;
extern u16 timingFrames;
extern s8 temp_offset;
extern u8 render_blended;
extern u8 msx_dos22;
extern u32 dos22_bank;
extern u16 zone_terra_hack;
extern u8 turbo_r_boost;
extern u8 SpecialPage[256];

extern FI_MSX gpFic[MAX_FILES];
extern int ucGameAct;
extern int ucGameChoice;

extern u8 msx_caps_lock;
extern u8 msx_kana_lock;
extern u8 mirror_ram_bank[4];

extern u8 zanac_glitch_patch;

// Mapper Types
#define GUESS           0
#define KON8            1
#define ASC8            2
#define SCC8            3
#define ASC16           4
#define ASC16X          5
#define NEO8            6
#define NEO16           7
#define MSXDOS2         8
#define PAC_SRAM        9

#define END_MAPPERS     10

#define MAX_GUESS_MAPPER 8   // The highest guess we can guess when examining ROM data

#define    MAX_CART_SIZE_KB     4096    // 4MB Cart
#define    MAX_DISK_SIZE_KB     720     // Standard DSDD
#define    RAM_SIZE_KB          2048    // 2MB RAM

extern u8 *ROM_Memory;
extern u8 *DISK_Memory;
extern u8 *DISK_Memory1;
extern u8 *DISK_Memory2;
extern u8 *RAM_Memory;
extern u8 BIOS_Memory[0x8000];
extern u8 SRAM_Memory[0x8000];

extern u8 TURBO_BIOS[32*1024];
extern u8 TURBO_SUB[16*1024];
extern u8 TURBO_MSXMUSIC[16*1024];
extern u8 TURBO_DISK[16*1024];
extern u8 TURBO_KANJI[32*1024];
extern u8 TURBO_OPENING[16*1024];
extern u8 KANJI_FONT[256*1024];
extern u8 TURBO_DOS22[64*1024];
extern u8 TURBO_DOS23[64*1024];
extern void msx_set_turbo_dos2_bank(u8 bank);

#define ASCII16X_CFI_SIZE                 0x90
#define ASCII16X_CFI_BYTE(offset)         ((offset) * 2)
#define ASCII16X_COMMAND_ADDRESS(address) ((address) & 0x0FFE)

extern u8 s1990_is_r800;
extern u8 s1990_reg6;

extern u8 bCartInPage[4];
extern u8 bRAMInPage[4];
extern u8 *MSXCartPtr[2][8];
extern u8 *MSXRamPtr[8];
extern u8 *MemoryMap[8];
extern AY38910 myAY;
extern YM      myYM;
extern SCC     mySCC;
extern u8 JoyMode;
extern u8 bClickRemoveNeeded;
extern u32 JoyState;
extern u8 Port_PPI_A;
extern u8 Port_PPI_B;
extern u8 Port_PPI_C;
extern void ProcessBufferedKeys(void);
extern u8 BufferedKeys[32];
extern u8 BufferedKeysWriteIdx;
extern u8 BufferedKeysReadIdx;
extern u8 key_shift;
extern u8 key_ctrl;
extern u8 key_kana;
extern u8 key_graph;
extern u8 msx_music_capable_game;
extern u8 msx_scc_capable_game;
extern u8 msx_subslot3;
extern u8 msx_subslot0;
extern u8 XPalReal0;
extern u8 ALatch;
extern u32 frame_number;
extern u8 CurrentEpochSaved;
extern u8 msx_irq_pending;
extern u8 palette_latch;
extern u8 OccBuf[320];
extern u8 zoom_screen;
extern u8 keyMapType;
extern u8 bMainMenuActive;

// --------------------------------------------------
// Some CPU and VDP and SGM stuff that we need
// --------------------------------------------------
extern void Loop9938(void);
extern void msxUpdateScreen(void);
extern void LoadFileAndComputeCRC(u8 media_id);
extern void msxLoadState();
extern void msxSaveState();
extern void msxWipeRAM(void);
extern void msx_reset(void);
extern void msx_restore_bios(void);
extern void BufferKey(u8 key);
extern void BufferKeys(char *str);
extern void MSX_InitialMemoryLayout(void);
extern void msxSaveSRAM(void);
extern void msxLoadSRAM(void);
extern void Z80_Interface_Reset(void);
extern void LoadFavorites(void);
extern void preserveCompressedMem(void);
extern void restoreCompressedMem(void);
extern void HandleSCCPlusModeRegister(u8 value);
extern void SCC_LegacyWrite(u8 value, u16 address);
extern void BuildScreen8ColorMap(void);
extern void LoadConfigDatabase(void);
extern void VroomDSFindFiles(u8 media_id);
extern void VroomDSMainMenu(void);
extern void DSPrint(int iX,int iY,int iScr,char *szMessage);
extern void DSPrint_fps(u16 fps);
extern void VroomDSChangeKeymap(void);
extern void VroomDSGameOptions(bool);
extern void FadeToColor(unsigned char ucSens, unsigned short ucBG, unsigned char ucScr, unsigned char valEnd, unsigned char uWait);
extern void DisplayFileNames(void);
extern u32  ReadFileCarefully(char *filename, u8 *buf, u32 buf_size, u32 buf_offset, u32 *crc);
extern u8   VroomDSChooseFile(u8 media_id, u8 allow_dir_change);
extern u8   showMessage(char *szCh1, char *szCh2);
extern u32  LoopZ80(void);
extern void msxInit(void);
extern void SaveConfig(bool bShow);
extern void IndirectRegWrite9938(u8 Value);
extern void EnsureSaveDirectory(void);
extern void intro_logo(void);
extern void UpperScreenMainMenu(void);
extern void MsxSaveFlash(void);
extern void MsxRestoreFlash(void);
extern void Mouse_PollHost(void);
extern void SCC_LegacyWrite(u8 value, u16 address);
extern void msx_slot_map_turboR(unsigned char Value);
extern void UpdateSpecialPages(void);
extern void FlipScreenBuffer(u8 flip_buffer);
extern void setup_emulation_Blend(void);
extern void setup_emulation_NoBlend(void);

#endif
