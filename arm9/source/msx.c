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

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fat.h>
#include <dirent.h>

#include "Vroom-DS.h"
#include "CRC32.h"
#include "cpu/z80/Z80_interface.h"
#include "MSX_generic.h"
#include "fdc_toshiba.h"
#include "printf.h"

// The S1990 controller and BIOS will put shadow ROM just before RAM end
#define SHADOW_MAIN  (RAM_SIZE_KB-64)
#define SHADOW_SUB   (RAM_SIZE_KB-32)
#define SHADOW_KANJI (RAM_SIZE_KB-16)

unsigned char TURBO_BIOS[32*1024];
unsigned char TURBO_SUB[16*1024];
unsigned char TURBO_MSXMUSIC[16*1024];
unsigned char TURBO_KANJI[32*1024];
unsigned char KANJI_FONT[256*1024];
unsigned char TURBO_OPENING[16*1024];
unsigned char TURBO_DOS23[64*1024];


// -------------------------------------------------------------
// Some MSX Mapper / Slot Handling and sound generation stuff...
// -------------------------------------------------------------
u8 bCartInPage[4]           __attribute__((aligned(4))) __attribute__((section(".dtcm"))) = {0,0,0,0};
u8 bRAMInPage[4]            __attribute__((aligned(4))) __attribute__((section(".dtcm"))) = {0,0,0,0};

u8 *MSXCartPtr[2][8]        __attribute__((aligned(4))) __attribute__((section(".dtcm"))) = { {0,0,0,0,0,0,0,0}, {0,0,0,0,0,0,0,0} };
u8 *MSXRamPtr[8]            __attribute__((aligned(4))) __attribute__((section(".dtcm"))) = {0,0,0,0,0,0,0,0};

u8 msx_music_capable_game   __attribute__((section(".dtcm"))) = 0;      // True if game is making use of MSX-MUSIC (YM Sound)
u8 msx_scc_capable_game     __attribute__((section(".dtcm"))) = 0;      // True if game is making use of Konami SCC
u8 special_memory_access    __attribute__((section(".dtcm"))) = 0x00;   // This lets us know we've got something special mapped into memory
u32 msx_music_writes        __attribute__((section(".dtcm"))) = 0;      // If we cross a threshold, we decare we are MSX-MUSIC capable (and mix in the proper sound)
u8 msx_subslot3             __attribute__((section(".dtcm"))) = 0x00;   // Only one slot is expanded so we only need to track one register here
u8 msx_subslot0             __attribute__((section(".dtcm"))) = 0x00;   // Only one slot is expanded so we only need to track one register here
u16 turbo_timer             __attribute__((section(".dtcm"))) = 0x0000; // Rough Turbo-R timer handling - advanced 16 ticks every scanline.
u8 last_port_write          __attribute__((section(".dtcm"))) = 0x00;

SCC     mySCC               __attribute__((section(".dtcm")));          // Declare new SCC module for Konami MSX games that use it
AY38910 myAY                __attribute__((section(".dtcm")));          // Declare new AY structure for basic MSX sounds
YM      myYM                __attribute__((section(".dtcm")));          // Declare new YM module (MSX-MUSIC) for MSX games that use it

// Some Turbo-R Specific handlers... very bare bones!
u8 last_f4 = 0x00;
u8 last_port_41 = 0xFF;
u32 dos22_bank = 0;
u8 pcm_counter = 0;
u8 pcm_last_a4 = 0;
u8 pcm_last_a5 = 0;
u8 pcm_samples[PCM_SAMPLE_BUFFER_SIZE];
volatile u16 pcm_sample_read __attribute__((section(".dtcm"))) = 0;
volatile u16 pcm_sample_write __attribute__((section(".dtcm"))) = 0;
volatile u8 pcm_sample_delay_frames __attribute__((section(".dtcm"))) = 0;
volatile u8 pcm_sample_silence_frames __attribute__((section(".dtcm"))) = PCM_SAMPLE_SILENCE_FRAMES;
volatile u8 pcm_sample_frame_active __attribute__((section(".dtcm"))) = 0;
u8 pcm_sample_current __attribute__((section(".dtcm"))) = 128;
s32 pcm_sample_current_scaled __attribute__((section(".dtcm"))) = 0;

// Provides the "PAC OPPL" string needed for some games to detect SRAM
u8 FAKE_FMPAC[0x2000];

u8 FAKE_FMPAC_SIG[] = {
    0x00, 0x00, 0x00, 0x00, 0x82, 0x40, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x50, 0x41, 0x43, 0x32, 0x4F, 0x50, 0x4C, 0x4C,
};

// --------------------------------------------------------------------------
// These aren't used very often so we don't need them in fast .dtcm memory
// --------------------------------------------------------------------------
u16 msx_init            = 0x4000;
u16 msx_basic           = 0x0000;
u8 mirror_ram_bank[4]   = {3,2,1,0}; // For port readback mostly... though not all MSX2 machines report it back.

u8 Unmapped_Memory[0x2000]; // Full of 0xFF values. We point all memory segments here that are not mapped to some other device.

static uint8_t rtc_reg  = 0;      // Selected register index (0-15)
static uint8_t rtc_bank = 0;      // Active bank selected by Reg 13 (0-3)

// Complete 4-bank RAM array initialized with valid MSX2 checksums
static uint8_t rtc_ram[4][16] = {
    // Bank 0: Time & Date Tracking
    { 0,0,0,0, 0,0,0,0, 0,0,0,0, 0, 0x01, 0, 0 },

    // Bank 1: Alarm Settings (Do not place System/Screen settings here)
    { 0,0,0,0, 0,0,0,0, 0,0,0,0, 0, 0, 0, 0 },

    // Bank 2: MSX system initialization settings
    {
        0x0A,  // Reg 0: RTC settings valid — REQUIRED
        0x00,  // Reg 1: X adjust
        0x00,  // Reg 2: Y adjust
        0x00,  // Reg 3: SCREEN 0, normal display
        0x08,  // Reg 4: WIDTH low nibble
        0x02,  // Reg 5: WIDTH high bits (40 columns)
        0x0F,  // Reg 6: Initial text color
        0x04,  // Reg 7: Initial background color
        0x04,  // Reg 8: Initial border color
        0x00,  // Reg 9: Default transfer/keyboard options
        0x00,  // Reg 10: BEEP settings
        0x00,  // Reg 11: Startup logo colors
        0x01,  // Reg 12: Area code (US)
        0x00, 0x00, 0x00
    },

    // Bank 3: Color Palette Settings + General Validation Checksums
    { 0x04, 0x04, 0x07, 0x05, 0x02, 0x02, 0x07, 0x07,
      0x01, 0x01, 0x03, 0x03, 0x08, 0x05, 0x00, 0x00 }
};

// WRITE PORT 0xB4: Selects Register Index ONLY
__attribute__((noinline)) void write_port_RTC_index(uint8_t data)
{
    rtc_reg = data & 0x0F; // Only lower 4 bits pick the register!
}

// WRITE PORT 0xB5: Writes Data into the current Bank & Register
__attribute__((noinline))  void write_port_RTC_data(uint8_t data)
{
    uint8_t val = data & 0x0F;

    // Store data in current active bank
    rtc_ram[rtc_bank][rtc_reg] = val;

    // IF writing to Register 13 (Mode Register), update the ACTIVE BANK!
    if (rtc_reg == 13 || rtc_reg == 0x0D) {
        rtc_bank = val & 0x03; // Bits 0-1 select Banks 0-3
    }
}

// RTC Register Layout Constants for the date/time handling
#define R_1_SEC    0
#define R_10_SEC   1
#define R_1_MIN    2
#define R_10_MIN   3
#define R_1_HOUR   4
#define R_10_HOUR  5
#define R_WEEK     6
#define R_1_DAY    7
#define R_10_DAY   8
#define R_1_MON    9
#define R_10_MON   10
#define R_1_YEAR   11
#define R_10_YEAR  12

/**
 * Splits a decimal value into individual 4-bit BCD nibbles
 * and stores them sequentially into the RTC RAM array.
 */
void store_nibbles(uint8_t rtc_sub_ram[], int index_1, int index_10, int val)
{
    rtc_sub_ram[index_1]  = (uint8_t)(val % 10);        // Lower 4-bits (Units)
    rtc_sub_ram[index_10] = (uint8_t)((val / 10) % 10); // Upper 4-bits (Tens)
}

// ---------------------------------------------------------------------------------------
// Grab system time/date and populate RTC registers... unlikely any program really cares.
// ---------------------------------------------------------------------------------------
__attribute__((noinline)) void populate_msx_rtc()
{
    time_t now = time(NULL);
    struct tm *t = localtime(&now);

    // 1. Time Components
    store_nibbles(rtc_ram[0], R_1_SEC, R_10_SEC, t->tm_sec);
    store_nibbles(rtc_ram[0], R_1_MIN, R_10_MIN, t->tm_min);
    store_nibbles(rtc_ram[0], R_1_HOUR, R_10_HOUR, t->tm_hour);

    // 2. Day of the Week (0 = Sunday, 6 = Saturday)
    // Matches the native C struct tm standard perfectly for MSX2.
    rtc_ram[0][R_WEEK] = (uint8_t)t->tm_wday;

    // 3. Calendar Components (struct tm uses 0-11 for months; RP5C01 expects 1-12)
    store_nibbles(rtc_ram[0], R_1_DAY, R_10_DAY, t->tm_mday);
    store_nibbles(rtc_ram[0], R_1_MON, R_10_MON, t->tm_mon + 1);

    // 4. Year Tracking (2-digit, 00-99)
    int year_2_digits = t->tm_year % 100;
    store_nibbles(rtc_ram[0], R_1_YEAR, R_10_YEAR, year_2_digits);
}

// READ PORT 0xB5: Reads Data from current Bank & Register
__attribute__((noinline)) uint8_t read_port_RTC_data(void)
{
    populate_msx_rtc(); // Get current system time/date and populate RTC

    uint8_t val = rtc_ram[rtc_bank][rtc_reg] & 0x0F;

    // Reg 13 (Mode Register): Force BUSY flag (Bit 1) to 0
    if (rtc_reg == 13)
    {
        val &= ~0x02; // Bit 1 = 0 (NOT BUSY)
    }

    return val;
}

// -------------------------------------------------------------------------------------------------
// S1990 Bus Controller
// -------------------------------------------------------------------------------------------------

u8 s1990_reg_idx = 0;

// Register 6: default power-on state = Z80 ROM mode.
u8 s1990_reg6 = 0x60;

// 0 = Z80, 1 = R800
u8 s1990_is_r800 = 0;

// Register 5: internal software switch (0=right, 1=left).
// Set this according to the emulated machine's physical switch.
u8 s1990_reg5 = 0;

// -------------------------------------------------------------------------------------------------
// Port E4: Register selection
// -------------------------------------------------------------------------------------------------
__attribute__((noinline)) void write_port_E4(u8 Value)
{
    s1990_reg_idx = Value;
}


// -------------------------------------------------------------------------------------------------
// Port E5: Register access
// -------------------------------------------------------------------------------------------------
__attribute__((noinline)) u8 read_port_E5(void)
{
    switch(s1990_reg_idx)
    {
        case 5:
            return (s1990_reg5 & 0x40);

        case 6:
            return s1990_reg6;

        case 13:
            return 0x03;

        case 14:
            return 0x2F;

        case 15:
            return 0x8B;

        default:
            return 0xFF;
    }
}

typedef struct {
    dpair PC, SP;
    pair AF, BC, DE, HL, AF1, BC1, DE1, HL1, IX, IY;
    u8 I, R, R_HighBit, IFF;
} CPURegs;   // adjust field types to match Z80 struct

static CPURegs saved_regs[2];

static inline void SaveRegs(CPURegs *d) {
    d->PC=CPU.PC; d->SP=CPU.SP; d->AF=CPU.AF; d->BC=CPU.BC; d->DE=CPU.DE; d->HL=CPU.HL;
    d->AF1=CPU.AF1; d->BC1=CPU.BC1; d->DE1=CPU.DE1; d->HL1=CPU.HL1;
    d->IX=CPU.IX; d->IY=CPU.IY; d->I=CPU.I; d->R=CPU.R; d->R_HighBit=CPU.R_HighBit; d->IFF=CPU.IFF;
}

static inline void LoadRegs(const CPURegs *s)
{
    CPU.PC = s->PC;   CPU.SP = s->SP;
    CPU.AF = s->AF;   CPU.BC = s->BC;   CPU.DE = s->DE;   CPU.HL = s->HL;
    CPU.AF1 = s->AF1; CPU.BC1 = s->BC1; CPU.DE1 = s->DE1; CPU.HL1 = s->HL1;
    CPU.IX = s->IX;   CPU.IY = s->IY;
    CPU.I = s->I;     CPU.R = s->R;     CPU.R_HighBit = s->R_HighBit;
    CPU.IFF = s->IFF;
}

__attribute__((noinline)) void write_port_E5(u8 Value)
{
    switch(s1990_reg_idx)
    {
        case 6:
            u8 old_reg6 = s1990_reg6;
            s1990_reg6 = Value & 0x60;
            s1990_is_r800 = (s1990_reg6 & 0x20) ? 0:1;

            // -------------------------------------------------------------------
            // Are we switching CPUs? Save registers and restore the incoming CPU.
            // -------------------------------------------------------------------
            if (turbo_r_mode != s1990_is_r800)
            {
                SaveRegs(&saved_regs[turbo_r_mode]);      // departing CPU, old mode
                LoadRegs(&saved_regs[s1990_is_r800]);      // arriving CPU
                turbo_r_mode = s1990_is_r800;
            }
            
            if ((old_reg6 ^ s1990_reg6) & 0x40)            // DRAM bit changed, either direction
            {
                // -------------------------------------------------------------------------
                // If DRAM changed, we need to remap the pages as some of the System ROMs
                // might have shifted into DRAM or we might be back to normal ROM handling.
                // -------------------------------------------------------------------------
                msx_slot_map_turboR(Port_PPI_A);
            }
            break;

        default:
            // Other registers are read-only or unimplemented.
            break;
    }
}




// ------------------------------------------------------------------------------------------
// Keyboard / Joystick reading is generally done every frame so at most 60 times per second.
// It's fine to keep this out of ITCM fast memory as it's not a hot spot for emulation.
// ------------------------------------------------------------------------------------------
__attribute__((noinline)) u8 readport_keyboard(void)
{
      // ----------------------------------------------------------
      // Keyboard Port (Japanese Layout)
      //  Row   Bit_7 Bit_6 Bit_5 Bit_4 Bit_3 Bit_2 Bit_1 Bit_0
      //   0     "7"   "6"   "5"   "4"   "3"   "2"   "1"   "0"
      //   1     ":"   ";"   "["   YEN   "="   "-"   "9"   "8"
      //   2     "B"   "A"  DEAD   /     "."   ","   "]"  QUOTE
      //   3     "J"   "I"   "H"   "G"   "F"   "E"   "D"   "C"
      //   4     "R"   "Q"   "P"   "O"   "N"   "M"   "L"   "K"
      //   5     "Z"   "Y"   "X"   "W"   "V"   "U"   "T"   "S"
      //   6     F3    F2    F1   KANA   CAPS GRAPH CTRL  SHIFT
      //   7     RET   SEL   BS   STOP   TAB   ESC   F5    F4
      //   8    RIGHT DOWN   UP   LEFT   DEL   INS  HOME  SPACE
      // ----------------------------------------------------------

      // For the full keyboard overlay... this is a bit of a hack for SHIFT and CTRL
      if (last_special_key != 0)
      {
          if ((last_special_key_dampen > 0) && (last_special_key_dampen != 20))
          {
              if (--last_special_key_dampen == 0)
              {
                  last_special_key = 0;
              }
          }

          if (last_special_key == KBD_KEY_SHIFT)
          {
            key_shift = 1;
          }
          else if (last_special_key == KBD_KEY_CTRL)
          {
            key_ctrl = 1;
          }
          else if (last_special_key == KBD_KEY_KANA)
          {
            key_kana = 1;
          }
          else if (last_special_key == KBD_KEY_GRAPH)
          {
            key_graph = 1;
          }

          if ((kbd_key != 0) && (kbd_key != KBD_KEY_SHIFT) && (kbd_key != KBD_KEY_CTRL) && (kbd_key != KBD_KEY_KANA) && (kbd_key != KBD_KEY_GRAPH))
          {
              if (last_special_key_dampen == 20) last_special_key_dampen = 19;    // Start the SHIFT/CONTROL countdown... this should be enough time for it to register
          }
      }

      u8 key1 = 0x00;   // Accumulate keys here...

      // -------------------------------------------------
      // Check every key that might have been pressed...
      // -------------------------------------------------
      for (u8 i=0; i< (kbd_keys_pressed ? kbd_keys_pressed:1); i++) // Always one pass at least for joysticks...
      {
          kbd_key = kbd_keys[i];

          if ((Port_PPI_C & 0x0F) == 0)      // Row 0
          {
              if (kbd_key)
              {
                  if (kbd_key == '0')           key1  |= 0x01;
                  if (kbd_key == '1')           key1  |= 0x02;
                  if (kbd_key == '2')           key1  |= 0x04;
                  if (kbd_key == '3')           key1  |= 0x08;
                  if (kbd_key == '4')           key1  |= 0x10;
                  if (kbd_key == '5')           key1  |= 0x20;
                  if (kbd_key == '6')           key1  |= 0x40;
                  if (kbd_key == '7')           key1  |= 0x80;
              }
          }
          else if ((Port_PPI_C & 0x0F) == 1)  // Row 1
          {
              if (kbd_key)
              {
                  if (kbd_key == '8')           key1 |= 0x01;
                  if (kbd_key == '9')           key1 |= 0x02;
                  if (kbd_key == '-')           key1 |= 0x04;
                  if (kbd_key == '=')           key1 |= 0x08;
                  if (kbd_key == '|')           key1 |= 0x10; // YEN
                  if (kbd_key == '@')           key1 |= 0x20;
                  if (kbd_key == '[')           key1 |= 0x40;
                  if (kbd_key == ';')           key1 |= 0x80;
                  if (kbd_key == ':')           key1 |= 0x80;
              }
          }
          else if ((Port_PPI_C & 0x0F) == 2)  // Row 2
          {
              if (kbd_key)
              {
                  if (kbd_key == KBD_KEY_QUOTE) key1 |= 0x01;
                  if (kbd_key == ']')           key1 |= 0x02;
                  if (kbd_key == ',')           key1 |= 0x04;
                  if (kbd_key == '.')           key1 |= 0x08;
                  if (kbd_key == '/')           key1 |= 0x10;
                  if (kbd_key == KBD_KEY_DEAD)  key1 |= 0x20;
                  if (kbd_key == 'A')           key1 |= 0x40;
                  if (kbd_key == 'B')           key1 |= 0x80;
              }
          }
          else if ((Port_PPI_C & 0x0F) == 3)  // Row 3
          {
              if (kbd_key)
              {
                  if (kbd_key == 'C')           key1 |= 0x01;
                  if (kbd_key == 'D')           key1 |= 0x02;
                  if (kbd_key == 'E')           key1 |= 0x04;
                  if (kbd_key == 'F')           key1 |= 0x08;
                  if (kbd_key == 'G')           key1 |= 0x10;
                  if (kbd_key == 'H')           key1 |= 0x20;
                  if (kbd_key == 'I')           key1 |= 0x40;
                  if (kbd_key == 'J')           key1 |= 0x80;
              }
          }
          else if ((Port_PPI_C & 0x0F) == 4)  // Row 4
          {
              if (kbd_key)
              {
                  if (kbd_key == 'K')           key1 |= 0x01;
                  if (kbd_key == 'L')           key1 |= 0x02;
                  if (kbd_key == 'M')           key1 |= 0x04;
                  if (kbd_key == 'N')           key1 |= 0x08;
                  if (kbd_key == 'O')           key1 |= 0x10;
                  if (kbd_key == 'P')           key1 |= 0x20;
                  if (kbd_key == 'Q')           key1 |= 0x40;
                  if (kbd_key == 'R')           key1 |= 0x80;
              }
          }
          else if ((Port_PPI_C & 0x0F) == 5)  // Row 5
          {
              if (kbd_key)
              {
                  if (kbd_key == 'S')           key1 |= 0x01;
                  if (kbd_key == 'T')           key1 |= 0x02;
                  if (kbd_key == 'U')           key1 |= 0x04;
                  if (kbd_key == 'V')           key1 |= 0x08;
                  if (kbd_key == 'W')           key1 |= 0x10;
                  if (kbd_key == 'X')           key1 |= 0x20;
                  if (kbd_key == 'Y')           key1 |= 0x40;
                  if (kbd_key == 'Z')           key1 |= 0x80;
              }
          }
          else if ((Port_PPI_C & 0x0F) == 6) // Row 6
          {
              if (kbd_key)
              {
                  if (kbd_key == KBD_KEY_SHIFT) key1 |= 0x01;
                  if (kbd_key == KBD_KEY_CTRL)  key1 |= 0x02;
                  if (kbd_key == KBD_KEY_GRAPH) key1 |= 0x04;
                  if (kbd_key == KBD_KEY_CAPS)  key1 |= 0x08;
                  if (kbd_key == KBD_KEY_KANA)  key1 |= 0x10;
                  if (kbd_key == KBD_KEY_F1)    key1 |= 0x20;
                  if (kbd_key == KBD_KEY_F2)    key1 |= 0x40;
                  if (kbd_key == KBD_KEY_F3)    key1 |= 0x80;
              }
              if (key_shift)  key1 |= 0x01;  // SHIFT
              if (key_ctrl)   key1 |= 0x02;  // CTRL
              if (key_graph)  key1 |= 0x04;  // GRAPH
              if (key_kana)   key1 |= 0x10;  // KANA
          }
          else if ((Port_PPI_C & 0x0F) == 7) // Row 7
          {
              if (kbd_key)
              {
                  if (kbd_key == KBD_KEY_F4)    key1 |= 0x01;
                  if (kbd_key == KBD_KEY_F5)    key1 |= 0x02;
                  if (kbd_key == KBD_KEY_ESC)   key1 |= 0x04;
                  if (kbd_key == KBD_KEY_TAB)   key1 |= 0x08;
                  if (kbd_key == KBD_KEY_STOP)  key1 |= 0x10;
                  if (kbd_key == KBD_KEY_BS)    key1 |= 0x20;
                  if (kbd_key == KBD_KEY_SEL)   key1 |= 0x40;
                  if (kbd_key == KBD_KEY_RET)   key1 |= 0x80;
              }
          }
          else if ((Port_PPI_C & 0x0F) == 8) // Row 8
          {
              if (kbd_key)
              {
                  if (kbd_key == ' ')           key1 |= 0x01;
                  if (kbd_key == KBD_KEY_HOME)  key1 |= 0x02;
                  if (kbd_key == KBD_KEY_INS)   key1 |= 0x04;
                  if (kbd_key == KBD_KEY_DEL)   key1 |= 0x08;
                  if (kbd_key == KBD_KEY_LEFT)  key1 |= 0x10;
                  if (kbd_key == KBD_KEY_UP)    key1 |= 0x20;
                  if (kbd_key == KBD_KEY_DOWN)  key1 |= 0x40;
                  if (kbd_key == KBD_KEY_RIGHT) key1 |= 0x80;
              }
          }
      }
      return ~key1;
}


__attribute__((noinline)) u8 ReadJoystick(void)
{
    u8 joy1 = 0;

    if (myConfig.dpad == DPAD_DIAGONALS)
    {
        if (JoyState & JST_UP)    joy1 |= (0x01 | 0x08);
        if (JoyState & JST_DOWN)  joy1 |= (0x02 | 0x04);
        if (JoyState & JST_LEFT)  joy1 |= (0x04 | 0x01);
        if (JoyState & JST_RIGHT) joy1 |= (0x08 | 0x02);

        if (JoyState & JST_FIRE1) joy1 |= 0x10;
        if (JoyState & JST_FIRE2) joy1 |= 0x20;
    }
    else // DPAD_NORMAL or SLIDE-N-GLIDE
    {
        if (JoyState & JST_UP)    joy1 |= 0x01;
        if (JoyState & JST_DOWN)  joy1 |= 0x02;
        if (JoyState & JST_LEFT)  joy1 |= 0x04;
        if (JoyState & JST_RIGHT) joy1 |= 0x08;

        if (JoyState & JST_FIRE1) joy1 |= 0x10;
        if (JoyState & JST_FIRE2) joy1 |= 0x20;
    }

    return joy1;
}


static u16 KanjiAdr = 0;       // Shared 12-bit character address
static u8  KanjiCounter = 0;   // Byte within 32-byte glyph
static u8  KanjiLevel = 0;     // Selected level: 0 or 1

__attribute__((noinline)) uint8_t ReadPort_KanjiDriver(uint8_t port)
{
    if (port == 0xD9 || port == 0xDB)
    {
        u32 offset = ((u32)KanjiLevel << 17)
                   + ((u32)KanjiAdr << 5)
                   + KanjiCounter;

        uint8_t data = KANJI_FONT[offset];

        KanjiCounter = (KanjiCounter + 1) & 0x1F;

        return data;
    }

    return 0xFF;
}

__attribute__((noinline)) void WritePort_KanjiDriver(uint8_t port, uint8_t value)
{
    switch (port)
    {
        case 0xD8:
        case 0xD9:
        case 0xDA:
        case 0xDB:
        {
            // Select Kanji level when writing either address port.
            KanjiLevel = (port & 2) ? 1 : 0;

            if (port & 1)
            {
                // Upper six bits of character address.
                KanjiAdr = (KanjiAdr & 0x03F)
                         | ((u16)(value & 0x3F) << 6);
            }
            else
            {
                // Lower six bits of character address.
                KanjiAdr = (KanjiAdr & 0xFC0)
                         | (value & 0x3F);
            }

            // Every address write resets the byte counter.
            KanjiCounter = 0;
            break;
        }
    }
}


typedef struct {
    s32 pendX, pendY;   // host motion not yet delivered to the game
    s16 latX, latY;     // values latched for the current 4-read sequence
    u8  phase;          // 0=X hi, 1=X lo, 2=Y hi, 3=Y lo
    u8  strobe;         // last pin 8 level
    u8  buttons;        // bit0 = left, bit1 = right (1 = pressed)
    u32 lastEdge;       // scanline stamp of the last strobe edge
} MouseState;

static MouseState mouse;

#define MOUSE_TIMEOUT_LINES 16   // ~1 ms at 63.5 us/line; tune if a game misses motion

void Mouse_Reset(void) { memset(&mouse, 0, sizeof(mouse)); }

#define MOUSE_DPAD_SPEED  2
#define MOUSE_PEND_MAX    (MOUSE_DPAD_SPEED * 2)   // never hold more than ~2 frames of motion

static u8 needLatch;

// once per frame
__attribute__((noinline)) void Mouse_PollHost(void)
{
    s32 dx = 0, dy = 0;
    if (JoyState & JST_LEFT)  dx -= MOUSE_DPAD_SPEED;
    if (JoyState & JST_RIGHT) dx += MOUSE_DPAD_SPEED;
    if (JoyState & JST_UP)    dy -= MOUSE_DPAD_SPEED;
    if (JoyState & JST_DOWN)  dy += MOUSE_DPAD_SPEED;

    mouse.pendX = dx ? mouse.pendX + dx : 0;
    mouse.pendY = dy ? mouse.pendY + dy : 0;

    if (mouse.pendX >  MOUSE_PEND_MAX) mouse.pendX =  MOUSE_PEND_MAX;
    if (mouse.pendX < -MOUSE_PEND_MAX) mouse.pendX = -MOUSE_PEND_MAX;
    if (mouse.pendY >  MOUSE_PEND_MAX) mouse.pendY =  MOUSE_PEND_MAX;
    if (mouse.pendY < -MOUSE_PEND_MAX) mouse.pendY = -MOUSE_PEND_MAX;
}

// R#15 write: only tracks the strobe, never consumes motion
__attribute__((noinline)) void Mouse_WritePortB(u8 r15, u32 now)
{
    u8 s = (r15 >> 4) & 1;          // port 1 pin 8, regardless of bit 6
    if (s == mouse.strobe) return;  // only real edges count
    mouse.strobe = s;

    if ((now - mouse.lastEdge) > MOUSE_TIMEOUT_LINES)
        mouse.phase = 3;            // idle: next edge becomes X high

    mouse.phase = (mouse.phase + 1) & 3;
    if (mouse.phase == 0) needLatch = 1;   // motion taken on the first read
    mouse.lastEdge = now;
}

// R#14 read: the first read of a sequence takes the motion
__attribute__((noinline)) u8 Mouse_Read(void)
{
    if (needLatch)
    {
        needLatch = 0;
        mouse.latX = -mouse.pendX;
        mouse.latY = -mouse.pendY;
        mouse.pendX = 0;
        mouse.pendY = 0;
    }

    u8 n;
    switch (mouse.phase)
    {
        case 0:  n = (mouse.latX >> 4) & 0x0F; break;
        case 1:  n =  mouse.latX       & 0x0F; break;
        case 2:  n = (mouse.latY >> 4) & 0x0F; break;
        default: n =  mouse.latY       & 0x0F; break;
    }
    u8 b = 0x30;
    if (JoyState & JST_FIRE1) b &= ~0x10;
    if (JoyState & JST_FIRE2) b &= ~0x20;
    return 0xC0 | b | n;
}

__attribute__((noinline)) void read_joystick_or_mouse(void)
{
    if ((myAY.ayPortBOut & 0x40) == 0)
    {
        if (myConfig.dpad == DPAD_MOUSE)
        {
            myAY.ayPortAIn = Mouse_Read();
        }
        else
        {
            myAY.ayPortAIn = ~ReadJoystick();
        }
    }
    else
    {
        myAY.ayPortAIn = 0xFF;
    }
}


// --------------------------------------------------------------------
// MSX IO Port Read - The MSX has a lot of I/O mapped peripherals
// including Joystick, PSG, SCC, RTC, keyboard, etc.
// --------------------------------------------------------------------
ITCM_CODE __attribute__((optimize("no-jump-tables"))) unsigned char cpu_readport_msx(register u8 Port)
{
  // These two are the most frequent so just handle them first...
  if (Port == 0x98) return RdData9938();               // VDP Data
  if (Port == 0x99) return RdCtrl9938();               // VDP Control (Status)

  switch (Port)
  {
    case 0xA9: return readport_keyboard();
    case 0xAA: return Port_PPI_C;
    case 0xA8: return Port_PPI_A;

    case 0xA2:  // PSG read... might be joypad data
      if (myAY.ayRegIndex == 14)      read_joystick_or_mouse();
      else if (myAY.ayRegIndex == 15) myAY.ayPortBIn = myAY.ayPortBOut;
      return ay38910DataR(&myAY);

    case 0xE6: return turbo_timer & 0xFF;
    case 0xE7: return (turbo_timer >> 8) & 0xFF;
    case 0xE4: return s1990_reg_idx;
    case 0xE5: return read_port_E5();
    case 0xE9: return myConfig.MIDI;
    case 0xA7: return 0x00;                            // Pause switch released
    case 0xF4: return last_f4;
    case 0xB5: return read_port_RTC_data();

    case 0xD8: case 0xD9: case 0xDA: case 0xDB:
      return ReadPort_KanjiDriver(Port);

    case 0xA4: return (pcm_counter & 3);
    case 0xA5:
    {
      byte comp = (0x80 >= pcm_last_a4) ? 0x80 : 0x00;
      return comp | (pcm_last_a5 & 0x1F);
    }
    case 0xF3:
      return ((VDP[0] >> 1) & 0x07) | (VDP[1] & 0x18) |
             (VDP[8] & 0x20) | ((VDP[25] & 0x18) << 3);

    case 0xFC: case 0xFD: case 0xFE: case 0xFF:
      return mirror_ram_bank[Port - 0xFC] | 0x80;     // Upper bits always set. In our case, we use all bits for paging except the top.

    default: return NORAM;
  }
}

// -----------------------------------------------------------------------------
// To avoid calling the overhead of readmem16() and writemem16() we keep pages
// of fast memory to indicate what 256 byte page is 'special' in that it might
// respond to special read/write handling. 98% of the time, it's not special
// so the Z80/R800 can just take the fast path and read directly without worry.
// -----------------------------------------------------------------------------
u8 SpecialPage[256] __attribute__((section(".dtcm"), aligned(4))) = {0};
u8 WriteSlow[256]   __attribute__((section(".dtcm"), aligned(4))) = {0};

ITCM_CODE void UpdateSpecialPages(void)
{
    static u32 lastKey = 0xFFFFFFFF; // So we only update when there is a change...

    const u32 sccOn  = (special_memory_access & SPEC_MEM_SCC_ENABLED) && bCartInPage[2];
    const u32 diskOn = special_memory_access & SPEC_MEM_DISK_CONTROLLER;
    const u32 diskOnH= special_memory_access & SPEC_MEM_DISK_CONTROLLER_HIGH;
    const u32 subOn  = special_memory_access & (SPEC_MEM_SUBSLOT0_ACTIVE | SPEC_MEM_SUBSLOT3_ACTIVE);

    u32 key = (sccOn ? 1 : 0) | (diskOn ? 2 : 0) | (subOn ? 4 : 0)
            | (bRAMInPage[0] ? 8 : 0)  | (bRAMInPage[1] ? 16 : 0)
            | (bRAMInPage[2] ? 32 : 0) | (bRAMInPage[3] ? 64 : 0) 
            | (diskOnH ? 128 : 0);
    
    if (key == lastKey) return;                 // nothing that matters changed
    lastKey = key;

    // ---- Reads: only 0x7F, 0x98-0x9F and 0xFF can ever be set ----
    u32 *r = (u32*)&SpecialPage[0x98];          // 0x98 is word-aligned
    u32 sccFill = sccOn ? 0x01010101 : 0;
    r[0] = sccFill; r[1] = sccFill;             // 9800-9FFF
    SpecialPage[0x7F] = diskOn ? 1 : 0;         // 7F00-7FFF
    SpecialPage[0xBF] = diskOnH? 1 : 0;         // BF00-7FFF
    SpecialPage[0xFF] = subOn  ? 1 : 0;         // FF00-FFFF

    // ---- Writes: four 16K pages x 64 entries, word fills ----
    // We assume every page that is not RAM should go through the slow write handler...
    u32 *w = (u32*)WriteSlow;
    for (int pg = 0; pg < 4; pg++)
    {
        u32 v = bRAMInPage[pg] ? 0 : 0x01010101;
        for (int i = 0; i < 16; i++) *w++ = v;
    }
    if (subOn) WriteSlow[0xFF] = 1;             // FFFF subslot register
}

// ------------------------------------------------------------------------------
// Slot map for this emulation roughly resembles the Panasonic A1GT machine but
// without the Panasonic mapper in slot 3-3 and with the PAC-SRAM in slot 2.
//
// bCartInPage[]/bRAMInPage[] are cleared in bulk up front. Only the spots that
// can be non-zero set them below.
// ------------------------------------------------------------------------------
void msx_slot_map_turboR(unsigned char Value)
{
    special_memory_access &=  ~(SPEC_MEM_SUBSLOT3_ACTIVE | SPEC_MEM_SUBSLOT0_ACTIVE | SPEC_MEM_DISK_CONTROLLER  | SPEC_MEM_DISK_CONTROLLER_HIGH);

    memset(bCartInPage, 0, sizeof(bCartInPage));
    memset(bRAMInPage,  0, sizeof(bRAMInPage));

    switch (Value & 0x03)  // Page 0 [0x0000~0x3FFF]
    {
        case 0x00:  // Slot 0:  Maps to Main BIOS ROM
            if ((msx_subslot0 & 0x03) == 0) // Subslot 0 has main ROM
            {
                if (s1990_reg6 == 0x00)
                {
                    MemoryMap[0] = RAM_Memory + (SHADOW_MAIN * 1024);
                    MemoryMap[1] = RAM_Memory + (SHADOW_MAIN * 1024);
                }
                else
                {
                    MemoryMap[0] = TURBO_BIOS;
                    MemoryMap[1] = TURBO_BIOS;
                }
            }
            else
            {
                MemoryMap[0] = Unmapped_Memory - 0x0000;
                MemoryMap[1] = Unmapped_Memory - 0x2000;
            }
            break;
        case 0x01:  // Slot 1:  Maps to Game Cart
            bCartInPage[0] = 1;
            MemoryMap[0] = (u8 *)(MSXCartPtr[MEDIA_CART1][0]) - 0x0000;
            MemoryMap[1] = (u8 *)(MSXCartPtr[MEDIA_CART1][1]) - 0x2000;
            break;
        case 0x02:  // Slot 2:  Secondary Cart Slot
            bCartInPage[0] = 2;
            MemoryMap[0] = (u8 *)(MSXCartPtr[MEDIA_CART2][0]) - 0x0000;
            MemoryMap[1] = (u8 *)(MSXCartPtr[MEDIA_CART2][1]) - 0x2000;
            break;
        case 0x03:  // Slot 3:  This is an expanded slot... has Extended BIOS and Disk Controller ROMs
            if ((msx_subslot3 & 0x03) == 1) // Subslot 1 has Extended BIOS
            {
                if (s1990_reg6 == 0x00)
                {
                    MemoryMap[0] = RAM_Memory + (SHADOW_SUB * 1024);
                    MemoryMap[1] = RAM_Memory + (SHADOW_SUB * 1024);
                }
                else
                {
                    MemoryMap[0] = (u8 *)TURBO_SUB;
                    MemoryMap[1] = (u8 *)TURBO_SUB;
                }
            }
            else if ((msx_subslot3 & 0x03) == 0) // Subslot 0 has RAM Mapper
            {
                bRAMInPage[0] = 1;
                MemoryMap[0] = (u8 *)(MSXRamPtr[0]) - 0x0000;
                MemoryMap[1] = (u8 *)(MSXRamPtr[1]) - 0x2000;
            }
            else // Other subslots have nothing in this page
            {
                MemoryMap[0] = Unmapped_Memory - 0x0000;
                MemoryMap[1] = Unmapped_Memory - 0x2000;
            }
            break;
    }

    switch ((Value>>2) & 0x03)  // Page 1 [0x4000~0x7FFF]
    {
        case 0x00:  // Slot 0:  Maps to Main BIOS ROM
        {
            const u32 sub = (msx_subslot0 & 0x0C) >> 2;
            if (sub == 0) // Subslot 0-0 has main ROM
            {
                if (s1990_reg6 == 0x00)
                {
                    MemoryMap[2] = RAM_Memory + (SHADOW_MAIN * 1024);
                    MemoryMap[3] = RAM_Memory + (SHADOW_MAIN * 1024);
                }
                else
                {
                    MemoryMap[2] = TURBO_BIOS;
                    MemoryMap[3] = TURBO_BIOS;
                }
            }
            else if (sub == 1) // Subslot 0-1 Unmapped
            {
                MemoryMap[2] = (u8 *)Unmapped_Memory - 0x4000;
                MemoryMap[3] = (u8 *)Unmapped_Memory - 0x6000;
            }
            else if (sub == 2) // Subslot 0-2 has MSX-MUSIC
            {
                MemoryMap[2] = TURBO_MSXMUSIC + 0x0000 - 0x4000;
                MemoryMap[3] = TURBO_MSXMUSIC + 0x2000 - 0x6000;
            }
            else // Subslot 0-3 has Opening ROM
            {
                MemoryMap[2] = TURBO_OPENING + 0x0000 - 0x4000;
                MemoryMap[3] = TURBO_OPENING + 0x2000 - 0x6000;
            }
            break;
        }
        case 0x01:  // Slot 1:  Maps to Game Cart
            bCartInPage[1] = 1;
            MemoryMap[2] = (u8 *)(MSXCartPtr[MEDIA_CART1][2]) - 0x4000;
            MemoryMap[3] = (u8 *)(MSXCartPtr[MEDIA_CART1][3]) - 0x6000;
            break;
        case 0x02:  // Slot 2:  Secondary Cart Slot
            bCartInPage[1] = 2;
            MemoryMap[2] = (u8 *)(MSXCartPtr[MEDIA_CART2][2]) - 0x4000;
            MemoryMap[3] = (u8 *)(MSXCartPtr[MEDIA_CART2][3]) - 0x6000;
            break;
        case 0x03:  // Slot 3:  Expanded slot has the Disk Controller in subslot 1
        {
            const u32 sub = (msx_subslot3 & 0x0C) >> 2;
            if (sub == 0) // Subslot 3-0 has RAM Mapper
            {
                bRAMInPage[1] = 1;
                MemoryMap[2] = (u8 *)(MSXRamPtr[2]) - 0x4000;
                MemoryMap[3] = (u8 *)(MSXRamPtr[3]) - 0x6000;
            }
            else if (sub == 1) // Subslot 3-1 has Kanji
            {
                if (s1990_reg6 == 0x00)
                {
                    MemoryMap[2] = RAM_Memory + (SHADOW_KANJI * 1024) - 0x4000;
                    MemoryMap[3] = RAM_Memory + (SHADOW_KANJI * 1024) - 0x4000;
                }
                else
                {
                    MemoryMap[2] = (u8 *)TURBO_KANJI+0x0000 - 0x4000;
                    MemoryMap[3] = (u8 *)TURBO_KANJI+0x2000 - 0x6000;
                }
            }
            else if (sub == 2) // Subslot 3-2 has the DOS mapper ROM
            {
                u8 *dos_rom = TURBO_DOS23;
                special_memory_access |= SPEC_MEM_DISK_CONTROLLER;
                MemoryMap[2] = dos_rom + (dos22_bank * 0x4000) - 0x4000;
                MemoryMap[3] = dos_rom + (dos22_bank * 0x4000) - 0x4000;
            }
            else // Subslot 3-2 without DOS 2.2, or subslot 3-3: nothing in this page
            {
                MemoryMap[2] = Unmapped_Memory - 0x4000;
                MemoryMap[3] = Unmapped_Memory - 0x6000;
            }
            break;
        }
    }

    switch ((Value>>4) & 0x03)  // Page 2 [0x8000~0xBFFF]
    {
        case 0x00:  // Slot 0:  Maps to nothing... 0xFF
            MemoryMap[4] = Unmapped_Memory - 0x8000;
            MemoryMap[5] = Unmapped_Memory - 0xA000;
            break;
        case 0x01:  // Slot 1:  Maps to Game Cart
            bCartInPage[2] = 1;
            MemoryMap[4] = (u8 *)(MSXCartPtr[MEDIA_CART1][4]) - 0x8000;
            MemoryMap[5] = (u8 *)(MSXCartPtr[MEDIA_CART1][5]) - 0xA000;
            break;
        case 0x02:  // Slot 2:  Secondary Cart Slot
            bCartInPage[2] = 2;
            MemoryMap[4] = (u8 *)(MSXCartPtr[MEDIA_CART2][4]) - 0x8000;
            MemoryMap[5] = (u8 *)(MSXCartPtr[MEDIA_CART2][5]) - 0xA000;
            break;
        case 0x03:  // Slot 3:  Expanded slot
        {
            const u32 sub = (msx_subslot3 & 0x30) >> 4;
            if (sub == 0) // Subslot 3-0 has RAM Mapper
            {
                bRAMInPage[2] = 1;
                MemoryMap[4] = (u8 *)(MSXRamPtr[4]) - 0x8000;
                MemoryMap[5] = (u8 *)(MSXRamPtr[5]) - 0xA000;
            }
            else if (sub == 1) // Subslot 3-1 has Kanji
            {
                MemoryMap[4] = (u8 *)TURBO_KANJI+0x4000 - 0x8000;
                MemoryMap[5] = (u8 *)TURBO_KANJI+0x6000 - 0xA000;
            }
            else if (sub == 2) // Subslot 3-2 has the DOS mapper ROM
            {
                special_memory_access |= SPEC_MEM_DISK_CONTROLLER_HIGH;
                MemoryMap[4] = Unmapped_Memory - 0x8000;
                MemoryMap[5] = Unmapped_Memory - 0xA000;
            }
            else
            {
                MemoryMap[4] = Unmapped_Memory - 0x8000;
                MemoryMap[5] = Unmapped_Memory - 0xA000;
            }
            break;
        }
    }

    switch ((Value>>6) & 0x03)  // Page 3 [0xC000~0xFFFF]
    {
        case 0x00:  // Slot 0:  Maps to nothing... 0xFF
            special_memory_access |= SPEC_MEM_SUBSLOT0_ACTIVE; // Opens up 0xFFFF
            MemoryMap[6] = Unmapped_Memory - 0xC000;
            MemoryMap[7] = Unmapped_Memory - 0xE000;
            break;
        case 0x01:  // Slot 1:  Maps to Game Cart
            bCartInPage[3] = 1;
            MemoryMap[6] = (u8 *)(MSXCartPtr[MEDIA_CART1][6]) - 0xC000;
            MemoryMap[7] = (u8 *)(MSXCartPtr[MEDIA_CART1][7]) - 0xE000;
            break;
        case 0x02:  // Slot 2:  Secondary Cart Slot
            bCartInPage[3] = 2;
            MemoryMap[6] = (u8 *)(MSXCartPtr[MEDIA_CART2][6]) - 0xC000;
            MemoryMap[7] = (u8 *)(MSXCartPtr[MEDIA_CART2][7]) - 0xE000;
            break;
        case 0x03:  // Slot 3: Expanded
            special_memory_access |= SPEC_MEM_SUBSLOT3_ACTIVE; // Opens up 0xFFFF
            if ((msx_subslot3 & 0xC0) == 0) // Subslot 3-0 has RAM Mapper
            {
                bRAMInPage[3] = 1;
                MemoryMap[6] = (u8 *)(MSXRamPtr[6]) - 0xC000;
                MemoryMap[7] = (u8 *)(MSXRamPtr[7]) - 0xE000;
            }
            else // Other subslots have nothing mapped in...
            {
                MemoryMap[6] = Unmapped_Memory - 0xC000;
                MemoryMap[7] = Unmapped_Memory - 0xE000;
            }
            break;
    }

    UpdateSpecialPages();
}

// -----------------------------------------------------------------------------------------------
// MSX IO Port Write - VDP and AY Sound Chip, Disk I/O, MSX2 Expanded Memory plus Slot Mapper $A8
// -----------------------------------------------------------------------------------------------
ITCM_CODE __attribute__((optimize("no-jump-tables"))) void cpu_writeport_msx(register u8 Port,register unsigned char Value)
{
    static u8 msx_music_register = 0;

    if      (Port == 0x98) WrData9938(Value);                  // VDP Data
    else if (Port == 0x99) WrCtrl9938(Value);                  // VDP Control
    else switch (Port)
    {
        case 0x9A: write_port_palette(Value); break;
        case 0x9B: IndirectRegWrite9938(Value); break;
        case 0xA0: ay38910IndexW(Value&0xF, &myAY); break;
        case 0xA1:
            ay38910DataW(Value, &myAY);
            if (myAY.ayRegIndex == 15) Mouse_WritePortB(myAY.ayPortBOut, CurLine);
            break;

        case 0xA8: msx_slot_map_turboR(Value); Port_PPI_A = Value; break;
        case 0xA9: Port_PPI_B = Value; break;
        case 0xAA:
            Port_PPI_C = Value;
            msx_caps_lock = ((Port_PPI_C & 0x40) ? 0:1);
            break;
        case 0xAB:
        {
            u8 bit = (Value & 0x0E) >> 1;
            if (Value & 1) Port_PPI_C |= (1 << bit);
            else           Port_PPI_C &= ~(1 << bit);
            msx_caps_lock = ((Port_PPI_C & 0x40) ? 0:1);
            break;
        }

        case 0xE6: case 0xE7: turbo_timer = 0; break;
        case 0xE4: s1990_reg_idx = Value & 0xF; break;
        case 0xE5: write_port_E5(Value); break;
        case 0xF4: last_f4 = Value; break;
        case 0xA4: pcm_last_a4 = Value; pcm_counter = 0x00; break;
        case 0xA5: pcm_last_a5 = Value; break;

        case 0xB4: write_port_RTC_index(Value); break;
        case 0xB5: write_port_RTC_data(Value); break;

        case 0xFC: case 0xFD: case 0xFE: case 0xFF:
        {
            u8 page = Port-0xFC;
            u8 bank = Value & ((RAM_SIZE_KB / 16) - 1);
            mirror_ram_bank[page] = bank;
            MSXRamPtr[(page*2)+0] = RAM_Memory + (0x4000 * bank);
            MSXRamPtr[(page*2)+1] = RAM_Memory + (0x4000 * bank) + 0x2000;
            msx_slot_map_turboR(Port_PPI_A);   // Enable the new map
            break;
        }

        case 0x41: last_port_41 = Value; break;
        case 0x7C: msx_music_register = Value; break;
        case 0x7D:
            if (++msx_music_writes == 10)
            {
                msx_music_capable_game = 1;
                bClickRemoveNeeded = 5;
            }
            YMWrite(Value, msx_music_register, &myYM);
            break;

        case 0xD8: case 0xD9: case 0xDA: case 0xDB:
            WritePort_KanjiDriver(Port, Value);
            break;

        default: break;
    }

    last_port_write = Port;
}

// --------------------------------------------------------------------------
// Try to guess the ROM type from the loaded binary... basically we are
// counting the number of load addresses that would access a mapper hot-spot.
// --------------------------------------------------------------------------
u8 MSX_GuessROMType(u32 size)
{
    u8 type = KON8;  // Default to Konami 8K mapper
    u32 guess[MAX_GUESS_MAPPER];

    // Check filesize... if really large, it can only be ASCII16, ASCII16-X, NEO8 or NEO16
    if (size >= (4096 * 1024))
    {
        if (memcmp(ROM_Memory+16, "ASCII16X", 8) == 0)
        {
            return ASC16X;
        }
        if (memcmp(ROM_Memory+16, "ROM_NEO8", 8) == 0)
        {
            return NEO8;
        }
        if (memcmp(ROM_Memory+16, "ROM_NE16", 8) == 0)
        {
            return NEO16;
        }

        // Otherwise, this is about all we can support...
        return ASC16;
    }

    memset(guess, 0x00, sizeof(guess));
    for (int i=0; i<size - 3; i++)
    {
        if (ROM_Memory[i] == 0x32)   // LD,A instruction
        {
            u16 value = ROM_Memory[i+1] + (ROM_Memory[i+2] << 8);
            switch (value)
            {
                case 0x5000:
                case 0x9000:
                case 0xb000:
                    guess[SCC8]++;
                    break;
                case 0x4000:
                case 0x8000:
                case 0xa000:
                    guess[KON8]++;
                    break;
                case 0x6800:
                case 0x7800:
                    guess[ASC8]++;guess[ASC8]++;
                    break;
                case 0x6000:
                    guess[KON8]++;
                    guess[ASC8]++;
                    guess[ASC16]++;
                    break;
                case 0x7000:
                    guess[SCC8]++;
                    guess[ASC8]++;
                    guess[ASC16]++;
                    break;
                case 0x77FF:
                    guess[ASC16]++;guess[ASC16]++;
                    break;
            }
        }
    }

    // Now pick the mapper that had the most Load addresses above...
    if      ((guess[ASC16] > guess[KON8]) && (guess[ASC16] > guess[SCC8]) && (guess[ASC16] > guess[ASC8]))    type = ASC16;
    else if ((guess[ASC8]  > guess[KON8]) && (guess[ASC8]  > guess[SCC8]) && (guess[ASC8] >= guess[ASC16]))   type = ASC8;      // ASC8 wins "ties" over ASC16
    else if ((guess[SCC8]  > guess[KON8]) && (guess[SCC8]  > guess[ASC8]) && (guess[SCC8]  > guess[ASC16]))   type = SCC8;
    else type = KON8;

    if (size == (64 * 1024)) type = ASC16;      // Big percentage of 64K mapper ROMs are ASCII16 so default to that and override below
    if (size >  (2048 * 1024)) type = ASC16;    // Anything this big has to be ASC16 or some mapper we don't handle yet

    return type;
}


/*********************************************************************************
 * We wipe main RAM with 0x00 values (helps with compression of save states even
 * if not truly accurate to real hardware) and SRAM is wiped to 0xFF values.
 ********************************************************************************/
void msxWipeRAM(void)
{
    memset(RAM_Memory, 0x00, (RAM_SIZE_KB*1024));
}

// -------------------------------------------------------------------
// Return 1 if the MyMedia[media_id].mapperType indicates we are a 16K banking cart...
// Otherwise the system will assume a standard 8K mapper.
// -------------------------------------------------------------------
u8 Is16kBanking(u8 media_id)
{
    if (MyMedia[media_id].mapperType == ASC16 || MyMedia[media_id].mapperType == ASC16X || MyMedia[media_id].mapperType == MSXDOS2 || MyMedia[media_id].mapperType == NEO16)
    {
        return 1;
    }
    else
    {
        return 0;
    }
}

// ----------------------------------------------------------------------------------------------------------------
// For Vroom-DS, we are only supporting the common mappers. This is not Hachibitto and full MSX/MSX2/2+ emulation.
// ----------------------------------------------------------------------------------------------------------------
void MSX_CartSetup(u8 media_id)
{
    u8 *CART_Memory = ROM_Memory + (media_id == MEDIA_CART1 ? 0 : ((MAX_CART_SIZE_KB/2)*1024));

    if ((MyMedia[media_id].filesize >= (16 * 1024)) && (MyMedia[media_id].filesize <= (MAX_CART_SIZE_KB * 1024)))   // We'll take anything between these two...
    {
        if ((MyMedia[media_id].mapperType == KON8) || (MyMedia[media_id].mapperType == SCC8))
        {
            MSXCartPtr[media_id][0] = (u8*)CART_Memory+0x4000;        // Segment 2 Mirror
            MSXCartPtr[media_id][1] = (u8*)CART_Memory+0x6000;        // Segment 3 Mirror
            MSXCartPtr[media_id][2] = (u8*)CART_Memory+0x0000;        // Segment 0 default
            MSXCartPtr[media_id][3] = (u8*)CART_Memory+0x2000;        // Segment 1 default
            MSXCartPtr[media_id][4] = (u8*)CART_Memory+0x4000;        // Segment 2 default
            MSXCartPtr[media_id][5] = (u8*)CART_Memory+0x6000;        // Segment 3 default
            MSXCartPtr[media_id][6] = (u8*)CART_Memory+0x0000;        // Segment 0 Mirror
            MSXCartPtr[media_id][7] = (u8*)CART_Memory+0x2000;        // Segment 1 Mirror
        }
        else if (MyMedia[media_id].mapperType == ASC8)
        {
            MSXCartPtr[media_id][0] = (u8*)CART_Memory+0x0000;        // Segment 0 default
            MSXCartPtr[media_id][1] = (u8*)CART_Memory+0x0000;        // Segment 0 default
            MSXCartPtr[media_id][2] = (u8*)CART_Memory+0x0000;        // Segment 0 default
            MSXCartPtr[media_id][3] = (u8*)CART_Memory+0x0000;        // Segment 0 default
            MSXCartPtr[media_id][4] = (u8*)CART_Memory+0x0000;        // Segment 0 default
            MSXCartPtr[media_id][5] = (u8*)CART_Memory+0x0000;        // Segment 0 default
            MSXCartPtr[media_id][6] = (u8*)CART_Memory+0x0000;        // Segment 0 default
            MSXCartPtr[media_id][7] = (u8*)CART_Memory+0x0000;        // Segment 0 default
        }
        else if (MyMedia[media_id].mapperType == ASC16)
        {
            MSXCartPtr[media_id][0] = (u8*)Unmapped_Memory;          // Segment Unmapped
            MSXCartPtr[media_id][1] = (u8*)Unmapped_Memory;          // Segment Unmapped
            MSXCartPtr[media_id][2] = (u8*)CART_Memory+0x0000;       // Segment 0 default
            MSXCartPtr[media_id][3] = (u8*)CART_Memory+0x2000;       // Segment 0 default
            MSXCartPtr[media_id][4] = (u8*)CART_Memory+0x0000;       // Segment 0 default
            MSXCartPtr[media_id][5] = (u8*)CART_Memory+0x2000;       // Segment 0 default
            MSXCartPtr[media_id][6] = (u8*)Unmapped_Memory;          // Segment Unmapped
            MSXCartPtr[media_id][7] = (u8*)Unmapped_Memory;          // Segment Unmapped
        }
        else if (MyMedia[media_id].mapperType == ASC16X) // Similar to ASC16 but mirrors (and can be bigger)
        {
            MSXCartPtr[media_id][0] = (u8*)CART_Memory+0x4000;       // Segment 0 default
            MSXCartPtr[media_id][1] = (u8*)CART_Memory+0x6000;       // Segment 0 default
            MSXCartPtr[media_id][2] = (u8*)CART_Memory+0x0000;       // Segment 0 default
            MSXCartPtr[media_id][3] = (u8*)CART_Memory+0x2000;       // Segment 0 default
            MSXCartPtr[media_id][4] = (u8*)CART_Memory+0x4000;       // Segment 0 default
            MSXCartPtr[media_id][5] = (u8*)CART_Memory+0x6000;       // Segment 0 default
            MSXCartPtr[media_id][6] = (u8*)CART_Memory+0x0000;       // Segment 0 default
            MSXCartPtr[media_id][7] = (u8*)CART_Memory+0x2000;       // Segment 0 default
        }
        else if (MyMedia[media_id].mapperType == NEO8)
        {
            MSXCartPtr[media_id][0] = (u8*)CART_Memory+0xC000;        // Segment 6 default
            MSXCartPtr[media_id][1] = (u8*)CART_Memory+0xE000;        // Segment 7 default
            MSXCartPtr[media_id][2] = (u8*)CART_Memory+0x0000;        // Segment 0 default
            MSXCartPtr[media_id][3] = (u8*)CART_Memory+0x2000;        // Segment 1 default
            MSXCartPtr[media_id][4] = (u8*)CART_Memory+0x4000;        // Segment 2 default
            MSXCartPtr[media_id][5] = (u8*)CART_Memory+0x6000;        // Segment 3 default
            MSXCartPtr[media_id][6] = (u8*)CART_Memory+0x8000;        // Segment 4 default
            MSXCartPtr[media_id][7] = (u8*)CART_Memory+0xA000;        // Segment 5 default
        }
        else if (MyMedia[media_id].mapperType == NEO16)
        {
            MSXCartPtr[media_id][0] = (u8*)CART_Memory+0xC000;       // Segment 3 default
            MSXCartPtr[media_id][1] = (u8*)CART_Memory+0xE000;       // Segment 3 default
            MSXCartPtr[media_id][2] = (u8*)CART_Memory+0x0000;       // Segment 0 default
            MSXCartPtr[media_id][3] = (u8*)CART_Memory+0x2000;       // Segment 0 default
            MSXCartPtr[media_id][4] = (u8*)CART_Memory+0x4000;       // Segment 1 default
            MSXCartPtr[media_id][5] = (u8*)CART_Memory+0x6000;       // Segment 1 default
            MSXCartPtr[media_id][6] = (u8*)CART_Memory+0x8000;       // Segment 2 default
            MSXCartPtr[media_id][7] = (u8*)CART_Memory+0xA000;       // Segment 2 default
        }
        else if (MyMedia[media_id].mapperType == MSXDOS2)
        {
            MSXCartPtr[media_id][0] = (u8*)Unmapped_Memory;          // Segment Unmapped
            MSXCartPtr[media_id][1] = (u8*)Unmapped_Memory;          // Segment Unmapped
            MSXCartPtr[media_id][2] = (u8*)CART_Memory+0x0000;       // Segment 0 default
            MSXCartPtr[media_id][3] = (u8*)CART_Memory+0x2000;       // Segment 0 default
            MSXCartPtr[media_id][4] = (u8*)Unmapped_Memory;          // Segment Unmapped
            MSXCartPtr[media_id][5] = (u8*)Unmapped_Memory;          // Segment Unmapped
            MSXCartPtr[media_id][6] = (u8*)Unmapped_Memory;          // Segment Unmapped
            MSXCartPtr[media_id][7] = (u8*)Unmapped_Memory;          // Segment Unmapped
        }
        else // Just put it in Pages 1+2
        {
            MSXCartPtr[media_id][0] = (u8*)CART_Memory+0x0000;        // Segment 0 Mirror
            MSXCartPtr[media_id][1] = (u8*)CART_Memory+0x2000;        // Segment 1 Mirror
            MSXCartPtr[media_id][2] = (u8*)CART_Memory+0x4000;        // Segment 2
            MSXCartPtr[media_id][3] = (u8*)CART_Memory+0x6000;        // Segment 3
            MSXCartPtr[media_id][4] = (u8*)CART_Memory+0x0000;        // Segment 0
            MSXCartPtr[media_id][5] = (u8*)CART_Memory+0x2000;        // Segment 1
            MSXCartPtr[media_id][6] = (u8*)CART_Memory+0x4000;        // Segment 2 Mirror
            MSXCartPtr[media_id][7] = (u8*)CART_Memory+0x6000;        // Segment 3 Mirror
        }

        // ---------------------------------------------------------------------
        // We now set our memory masks such that we page memory in properly...
        // ---------------------------------------------------------------------
        if (MyMedia[media_id].filesize <= (128 * 1024))
        {
            if (Is16kBanking(media_id))
                MyMedia[media_id].mapperMask = (MyMedia[media_id].filesize <= (64 * 1024)) ? 0x03:0x07;
            else
                MyMedia[media_id].mapperMask = (MyMedia[media_id].filesize <= (64 * 1024)) ? 0x07:0x0F;
        }
        else if (MyMedia[media_id].filesize <= (512 * 1024))
        {
            if (Is16kBanking(media_id))
                MyMedia[media_id].mapperMask = (MyMedia[media_id].filesize <= (256 * 1024)) ? 0x0F:0x1F;
            else
                MyMedia[media_id].mapperMask = (MyMedia[media_id].filesize <= (256 * 1024)) ? 0x1F:0x3F;
        }
        else if (MyMedia[media_id].filesize <= (1024 * 1024))
        {
            if (Is16kBanking(media_id))
                MyMedia[media_id].mapperMask = 0x3F;
            else
                MyMedia[media_id].mapperMask = 0x7F;
        }
        else if (MyMedia[media_id].filesize <= (2048 * 1024))
        {
            if (Is16kBanking(media_id))
                MyMedia[media_id].mapperMask = 0x7F;
            else
                MyMedia[media_id].mapperMask = 0xFF;
        }
        else // Must be 4096K...  really only the 16K banking works here...
        {
            MyMedia[media_id].mapperMask = 0xFF;
        }
    }
    else
    {
        // Size not right for MSX support... we've already pre-filled 0xFF so nothing more to do here... System will not run.
    }

    // --------------------------------------------------------------------------
    // Some mappers have 8K blocks, some have 16K blocks... sort that out here.
    // --------------------------------------------------------------------------
    MyMedia[media_id].blockSize = (Is16kBanking(media_id) ? 0x4000:0x2000);
}

u8 CheckForDos22(void)
{
    if (memcmp(DISK_Memory + 0x20, "VOL_ID", 6) == 0) return 1;
    else return 0;
}

// -------------------------------------------------------------------------
// Setup the initial MSX memory layout based on the size of the ROM loaded.
// -------------------------------------------------------------------------
void MSX_InitialMemoryLayout(void)
{
    // -----------------------------------------------------
    // Fill the unused memory buffer. Anything unmapped in
    // our memory mapping will point here and read 0xFF.
    // -----------------------------------------------------
    for (int i=0; i<0x2000; i++)
    {
        Unmapped_Memory[i] = 0xFF;
    }

    // -------------------------------------
    // Make sure the MSX ports are clear
    // -------------------------------------
    Port_PPI_A = 0x00;
    Port_PPI_B = 0x00;
    Port_PPI_C = 0x00;

    // ----------------------------------
    // Some helper variables for the MSX
    // ----------------------------------
    msx_music_writes = 0;
    special_memory_access = 0x00;
    msx_subslot3 = 0x00;
    msx_subslot0 = 0x00;

    // -----------------------------------------
    // Setup RAM/ROM pointers back to defaults
    // -----------------------------------------
    memset(bRAMInPage,  0, sizeof(bRAMInPage));   // Default to no RAM paged in until told so
    memset(bCartInPage, 0, sizeof(bCartInPage));  // Default to no ROM paged in until told so

    for (u8 i=0; i<8; i++)
    {
        MSXCartPtr[MEDIA_CART1][i] = Unmapped_Memory;          // Cart1 has nothing in it by default - will fill it in below
        MSXCartPtr[MEDIA_CART2][i] = Unmapped_Memory;          // Cart2 has nothing in it by default - will fill it in below
    }

    // ---------------------------------------------
    // RAM maps backwards... for historical reasons.
    // ---------------------------------------------
    MSXRamPtr[0] = RAM_Memory + 0xC000;
    MSXRamPtr[1] = RAM_Memory + 0xE000;
    MSXRamPtr[2] = RAM_Memory + 0x8000;
    MSXRamPtr[3] = RAM_Memory + 0xA000;
    MSXRamPtr[4] = RAM_Memory + 0x4000;
    MSXRamPtr[5] = RAM_Memory + 0x6000;
    MSXRamPtr[6] = RAM_Memory + 0x0000;
    MSXRamPtr[7] = RAM_Memory + 0x2000;

    // ---------------------------------------------
    // Restore the MSX BIOS and point to it
    // ---------------------------------------------
    msx_restore_bios();

    // ---------------------------------------------------------------------------------------
    // CART2 has the PAC_SRAM with 8K of SRAM available... not really a cart but close enough.
    // ---------------------------------------------------------------------------------------
    MyMedia[MEDIA_CART2].mapperMask = 0x01;
    MyMedia[MEDIA_CART2].blockSize = 0x2000;
    MyMedia[MEDIA_CART2].mapperType = PAC_SRAM;
    
    memset(FAKE_FMPAC, 0xFF, sizeof(FAKE_FMPAC));

    // If we have been asked to fake the FM-PAC signature, do so... some
    // games like Illusion City require the PAC-OPPL signature to be present.
    if (myConfig.fakeFMPAC)
    {
        memcpy(FAKE_FMPAC, FAKE_FMPAC_SIG, sizeof(FAKE_FMPAC_SIG));
    }

    MSXCartPtr[MEDIA_CART2][0] = (u8*)Unmapped_Memory;           // Segment Unmapped
    MSXCartPtr[MEDIA_CART2][1] = (u8*)Unmapped_Memory;           // Segment Unmapped
    MSXCartPtr[MEDIA_CART2][2] = (u8*)FAKE_FMPAC;                // Segment Unmapped
    MSXCartPtr[MEDIA_CART2][3] = (u8*)Unmapped_Memory;           // Segment Unmapped
    MSXCartPtr[MEDIA_CART2][4] = (u8*)Unmapped_Memory;           // Segment Unmapped
    MSXCartPtr[MEDIA_CART2][5] = (u8*)Unmapped_Memory;           // Segment Unmapped
    MSXCartPtr[MEDIA_CART2][6] = (u8*)Unmapped_Memory;           // Segment Unmapped
    MSXCartPtr[MEDIA_CART2][7] = (u8*)Unmapped_Memory;           // Segment Unmapped

    msxLoadSRAM();  // FM-PAC with MSX-MUSIC also has 8K of SRAM

    if (MyMedia[MEDIA_CART1].filecrc) // For ROMs we need a mapper type... User might have pre-selected one or we need to guess.
    {
        if (myConfig.msxMapper == GUESS)
        {
            // Look for the matching SHA1 in the ROM Database
            if (MyMedia[MEDIA_CART1].filesize >= (64*1024))
            {
                MyMedia[MEDIA_CART1].mapperType = MSX_GuessROMType(MyMedia[MEDIA_CART1].filesize); // We only bother if the game is 64K or larger
            }

            // Still no idea... we just call it ASC8 and push on
            if (MyMedia[MEDIA_CART1].mapperType == 0xFF)
            {
                MyMedia[MEDIA_CART1].mapperType = ASC8;
            }
        }
        else // User has selected a specific mapper... who are we to argue?!
        {
            MyMedia[MEDIA_CART1].mapperType = myConfig.msxMapper;
        }

        MSX_CartSetup(MEDIA_CART1);
    }

    // ------------------------------------------------------------------------------------------------------------
    // If we are dealing with one of the rare FLASH writable games, read the modified flash blocks back into ROM.
    // ------------------------------------------------------------------------------------------------------------
    if (MyMedia[MEDIA_CART1].mapperType == ASC16X)
    {
        MsxRestoreFlash();
    }
}

// ---------------------------------------------------------------------------
// Classic Konami SCC register layout (relative to 9800h) puts the register
// block at 0x80 and shares one 32-byte waveform between Ch3 and Ch4 (the
// SCC+ driver instead treats 0x80-0x9F as Ch4's own independent wave RAM
// and moved the register block to 0xA0) - so classic-mode writes need to be
// relocated before reaching SCCWrite, and Ch3 wave writes need to be mirrored
// into Ch4's wave RAM to reproduce the real shared-waveform behavior.
// ---------------------------------------------------------------------------
void SCC_LegacyWrite(u8 value, u16 address)
{
    u8 off = address & 0xFF;

    if (off < 0x60)                        // Ch0-Ch2 wave RAM - untouched by the layout shift
    {
        SCCWrite(value, off, &mySCC);
    }
    else if (off < 0x80)                   // Ch3 wave RAM - also mirror into Ch4 (shared on real hardware)
    {
        SCCWrite(value, off, &mySCC);
        SCCWrite(value, off + 0x20, &mySCC);
    }
    else if (off < 0x90)                   // Old freq/vol/control block (0x80-0x8F) -> new 0xA0-0xAF block
    {
        SCCWrite(value, off + 0x20, &mySCC);
    }
    // Otherwise it's one of the mirrors or deformation registers which we aren't emulating yet...
}


// ---------------------------------------------------------------
// Restore the MSX BIOS into the memory buffer and point to it...
// ---------------------------------------------------------------
void msx_restore_bios(void)
{
    // Slot 0 - BIOS is mapped in to start...
    MemoryMap[0] = TURBO_BIOS + 0x0000 - 0x0000;
    MemoryMap[1] = TURBO_BIOS + 0x2000 - 0x2000;
    MemoryMap[2] = TURBO_BIOS + 0x4000 - 0x4000;
    MemoryMap[3] = TURBO_BIOS + 0x6000 - 0x6000;

    MemoryMap[4] = Unmapped_Memory - 0x8000;
    MemoryMap[5] = Unmapped_Memory - 0xA000;
    MemoryMap[6] = Unmapped_Memory - 0xC000;
    MemoryMap[7] = Unmapped_Memory - 0xE000;
}


// ---------------------------------------------------------
// The MSX reset has some special memory mapping depending
// on if we have loaded a .DSK or a .ROM file.
// ---------------------------------------------------------
void msx_reset(void)
{
    msx_init = 0x4000;
    msx_basic = 0x0000;
    last_f4 = 0x00;
    pcm_counter = 0;
    pcm_last_a4 = 0;
    pcm_last_a5 = 0;
    pcm_sample_read = 0;
    pcm_sample_write = 0;
    pcm_sample_delay_frames = 0;
    pcm_sample_silence_frames = PCM_SAMPLE_SILENCE_FRAMES;
    pcm_sample_frame_active = 0;
    pcm_sample_current = 128;
    pcm_sample_current_scaled = 0;
    s1990_is_r800 = 0;
    s1990_reg6 = 0x60;
    s1990_reg_idx = 0;
    s1990_reg5 = 0;
    turbo_r_mode = 0;
    msx_music_capable_game = 0;
    special_memory_access = 0x00;
    msx_music_writes = 0;
    msx_subslot3 = 0;
    msx_subslot0 = 0;
    turbo_timer = 0;
    dos22_bank = 0;
    KanjiAdr = 0;
    KanjiCounter = 0;
    KanjiLevel = 0;
    last_port_write = 0x00;
    
    mirror_ram_bank[0] = 3;
    mirror_ram_bank[1] = 2;
    mirror_ram_bank[2] = 1;
    mirror_ram_bank[3] = 0;
    
    MSX_InitialMemoryLayout();
    
    Mouse_Reset();
    
    memset(saved_regs, 0x00, sizeof(saved_regs));
    saved_regs[0].SP.W = 0xF000;
    saved_regs[1].SP.W = 0xF000;
    
    memset(SpecialPage, 0x00, sizeof(SpecialPage));
    memset(WriteSlow, 0x00, sizeof(WriteSlow));
    
    // -------------------------------------------------
    // If a disk was part of the setup, load it now...
    // -------------------------------------------------
    if (MyMedia[MEDIA_DISK].filecrc)
    {
        //2 sides * 80 tracks * 9 sectors per track * 512 bytes per sector = 737280 Bytes (720kB)
        fdc_toshiba_init(2, (MyMedia[MEDIA_DISK].filesize/1024 == 360) ? 1:2, 80, 9, 512, 1, DISK_Memory, DISK_Memory1);
        fdc_toshiba_reset(true);
    }
}

// -------------------------------------------------------------------
// For the few games that have SRAM built in. We back the SRAM onto
// a file with the same base filename but the extension is .SRM
// -------------------------------------------------------------------
extern char szName[];
void msxSaveSRAM(void)
{
    // Return to the original path
    chdir(GetMasterPath());

    // Make sure the 'sav' directory exists
    EnsureSaveDirectory();

    // Init filename = romname and SRM (SRAM) in place of ROM
    sprintf(szName,"sav/%s", GetMasterFilename());

    int len = strlen(szName);
    szName[len-3] = 's';
    szName[len-2] = 'r';
    szName[len-1] = 'm';
    szName[len-0] = 0;

    FILE *handle = fopen(szName, "wb+");
    if (handle != NULL)
    {
        fwrite(SRAM_Memory, sizeof(SRAM_Memory), 1, handle);
        fclose(handle);
    }
}

void msxLoadSRAM(void)
{
    // Return to the original path
    chdir(GetMasterPath());

    // Make sure the 'sav' directory exists
    EnsureSaveDirectory();

    // Init filename = romname and SRM (SRAM) in place of ROM
    sprintf(szName,"sav/%s", GetMasterFilename());

    int len = strlen(szName);
    szName[len-3] = 's';
    szName[len-2] = 'r';
    szName[len-1] = 'm';
    szName[len-0] = 0;

    // Check if the file exists... if so read it...
    if (ReadFileCarefully(szName, SRAM_Memory, sizeof(SRAM_Memory), 0, NULL) == 0)   // SRAM can be anywhere from 2K to 32K
    {
        memset(SRAM_Memory, 0xFF, sizeof(SRAM_Memory)); // Not found, so init the SRAM area
    }
}



// ---------------------------------------------------------------------------------
// Some of the newer Flash mappers are malleable - meaning that the flash sectors
// can be erased and reprogrammed while the game is running. The .rom file is
// somewhat more akin to a disk file in that it can be updated on-the-fly. If we
// have detected any sector erase and byte programming, we will start a 3 second
// countdown timer that will call into this routine to save the .rom file back out.
// ---------------------------------------------------------------------------------
static char szSaveFlashFile[MAX_FILE_NAME_LEN];

// -----------------------------------------------------------------------
// The flash save file is the same as the original ROM filename but we
// use .SST in place of .ROM and we also ensure the sav directory exists.
// -----------------------------------------------------------------------
static void MsxMakeFlashFilename(void)
{
    DIR* dir = opendir("sav");
    if (dir) closedir(dir);    // Directory exists... close it out and move on.
    else mkdir("sav", 0777);   // Otherwise create the directory...
    sprintf(szSaveFlashFile,"sav/%s", GetMasterFilename());

    int len = strlen(szSaveFlashFile);
    szSaveFlashFile[len-3] = 's';
    szSaveFlashFile[len-2] = 's';
    szSaveFlashFile[len-1] = 't';
}

// ---------------------------------------------------------------------------
// We save off only the modified 8K flash blocks and keep it separate from
// the original .rom file since we don't want to modify or change change the
// original CRC which has implications for config settings, etc. Plus it just
// feels cleaner to leave the original ROM as-is with no risk of damaging it.
// ---------------------------------------------------------------------------
void MsxSaveFlash(void)
{
    MsxMakeFlashFilename();
    FILE *handle = fopen(szSaveFlashFile, "wb");
    if (handle != NULL)
    {
        // -----------------------------------------------------------------------------
        // Write the 1024 byte writemap... this tells us what flash blocks were written.
        // -----------------------------------------------------------------------------
        fwrite(flashDirtyBlocks, sizeof(flashDirtyBlocks), 1, handle);

        // --------------------------------------------------------------------
        // And now write out all of the 8K blocks that were erased/written.
        // Generally not more than 1 or 2 blocks but we allow for any and all.
        // --------------------------------------------------------------------
        for (int i=0; i<1024; i++)
        {
            if (flashDirtyBlocks[i]) // Has block been modified?
            {
                fwrite(ROM_Memory + (0x2000 * i), 0x2000, 1, handle); // Write it...
            }
        }
        fclose(handle);
    }
}

// ---------------------------------------------------------------------------------------
// Restore the 8K blocks from the .sst file and place them back into the ROM_memory[].
// Think of the .sst file as the variable portion of the ROM that is written to save
// things like high scores or configuration for the game and is overlaid on the real ROM.
// ---------------------------------------------------------------------------------------
void MsxRestoreFlash(void)
{
    memset(flashDirtyBlocks, 0x00, sizeof(flashDirtyBlocks));

    MsxMakeFlashFilename();

    ReadFileCarefully(szSaveFlashFile, flashDirtyBlocks, sizeof(flashDirtyBlocks), 0, 0);

    // --------------------------------------------------------------------
    // Read back any modified blocks and put them into the proper place.
    // --------------------------------------------------------------------
    int offset = sizeof(flashDirtyBlocks);
    for (int i=0; i<1024; i++)
    {
        if (flashDirtyBlocks[i]) // Has 8K block been modified?
        {
            ReadFileCarefully(szSaveFlashFile, ROM_Memory + (0x2000 * i), 0x2000, offset, 0);
            offset += 0x2000;
        }
    }
}

// Ensure 'sav' directory exists. If not: create it.
void EnsureSaveDirectory(void)
{
    DIR* dir = opendir("sav");
    if (dir) closedir(dir);    // Directory exists... close it out and move on.
    else mkdir("sav", 0777);   // Otherwise create the directory...
}

// End of file
