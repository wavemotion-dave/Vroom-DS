# Vroom-DS - MSX Turbo-R Emulator for the DSi/XL/LL

Requires a DSi/XL/LL and will NOT run in DS-Lite or 'Compatibility' mode. The game
will check to make sure you have the needed processing power and RAM to run the game.

![Vroom-DS](arm9/gfx_data/topscreen.png)
![Vroom-DS](arm9/gfx_data/pdev_bg0.png)

# Donations Welcome (but never required!)

These hobby emulators have been a labor of love. An embarassing amount of development time has gone into it as I've strived to provide an enjoyable retro experience on the DS handheld. It's free to use and always will be, however if you feel inclined to buy me a virtual coffee for the effort, that would be beyond amazing!

[<img src="https://github.com/user-attachments/assets/ab67686c-2168-46a3-b39f-77b5f5fef5d0">](https://ko-fi.com/wavemotiondave)

Brief Instructions :
-----------------------
In addition to the Vroom-DS.nds file, you will need 3 files in the /roms/bios directory to use this emulator:

* fs-a1gt_firmware.rom (4MB) with CRC32 of 0xfeefeadc
* hb-f1xdj_kanjifont.rom (256K) with CRC32 of 0x7016dfd0
* ascii_msxdos22.rom (64K) with CRC32 of 0x47eea028

(for that last one, you can substitute MDOS22V3.ROM with CRC32 of 0x845f5ab6)
or
(for that last one, you can substitute MSXDOS22.ROM with CRC32 of 0x1c430991)

Copyright :
-----------------------
Vroom-DS is Copyright (c) 2026 Dave Bernazzani (wavemotion-dave)

As long as there is no commercial use (i.e. no profit is made),
copying and distribution of this emulator, it's source code
and associated readme files, with or without modification, 
are permitted in any medium without royalty provided this 
copyright notice is used and wavemotion-dave (Vroom-DS),
and Marat Fayzullin (fMSX core) are thanked profusely.

The sound driver (ay38910) are libraries from FluBBa (Fredrik Ahlström) 
and those copyrights remain his.

lzav compression (for save states) is Copyright (c) 2023-2025 Aleksey 
Vaneev and used by permission of the generous MIT license.

The Vroom-DS emulator is offered as-is, without any warranty.

Credit to @PantheonMusic - Opening "Fast Pace Dramatic Score" music

Virtual Slot Map :
-----------------------
![Slot Map](images/slotmap.png)

Known Issues :
-----------------------
* Speed is not fully accurate. It's the best I could do on the little handheld... some games play just fine and others will have slowdown.
* The MSX Translations version of Illusion City only runs in the 4MB ASC16X ROM version (which is more convenient anyway). You can use Flash or Disk to save progress.
* STG Special has major graphical glitches happening in the score area. Reason unknown.
* Don't try to run non-Turbo-R games on this thing... most of the MSX mappers are not present and SCC emulation is missing. Use Hachibitto for your general MSX emulation needs - this is to play around with Turbo R games.

Version History :
-----------------------
Version 0.5 - 13-Nov-2026 by wavemotion-dave
* First beta version released.

