# RRDMP — EX-word ROM32 and RAM16 dumper

This add-on is built using the original devkitSH4 compiler and original precompiled libdataplus libraries, because that combination works on the target hardware.

| Selection | Memory | Starting address | Ending address | Bytes |
|---|---|---|---|---|
| Enter | ROM32 | `0x80000000` | `0x81FFFFFF` | 33,554,432 |
| Page Down | RAM16 | `0x8C000000` | `0x8CFFFFFF` | 16,777,216 |
| S | SPU2/DSP snapshot | live SPU2 registers + mapped DSP/SPU windows | see below | 10 files |
| B | live DSP-beep snapshot | starts the ROM DSP beep, then captures its active state | see below | 10 files |

After selecting, **release the key and press Enter again** to confirm. Back exits. Files are saved to the SD card at `\\crd0\` with an `A`, `B`, or `C` suffix (for example `ROM32A.BIN`). Existing files are not deleted. A 16-byte SD filesystem probe is also available using T.

The code reads the selected ROM/RAM range and writes an ordinary file to SD; it does not deliberately write to firmware or RAM. The range may still include unmapped addresses and cause a reboot. Live RAM may change during capture and can contain private data. The image is **not validated on hardware** just because CI builds it.

The **S** snapshot is an audio-reverse-engineering mode. After confirmation it calls the ROM SPU2/DSP initializer at `0x80061334`, displays the four live bank-control values on screen, writes a named post-initialization shared-SPU2 register report (`SPU2RGA.TXT`, `SPU2RGB.TXT`, or `SPU2RGC.TXT`), then dumps only the mapped DSP0/DSP1 program, data, auxiliary, FSI, and DSP-control/DMA windows to new `D0*.BIN`, `D1*.BIN`, and `S2MBX*.BIN` files in `\\crd0\`. The `D1REG*.BIN` filename is retained for compatibility but maps the FSI peripheral at `0xFE3C0000`; DSP1 control/DMA registers live in `S2MBX*.BIN` at `0xFE3FF000`. The report includes `PBANKC0/1` and `XBANKC0/1` (the DSP0 program/data bank-layout controls), plus `SPUSRST`, `SPUADR`, `ENDIAN`, `GCOM0-7`, and `DMABUF0-3`. The complete raw shared-SPU2 page remains in `D0REGA.BIN` / `D0REGB.BIN`, covering `0xFE2FF000-0xFE2FFFFF`. DSP/SPU data is first read as volatile 32-bit values into application RAM, then written to SD; it is not handed directly to the OS file syscall as an MMIO pointer. It intentionally does not read across the unmapped gaps between those windows. This mode reinitializes the device audio subsystem and is intended for controlled test hardware only.

The **B** snapshot calls the exact ROM DSP1 beep start routine at `0x80061988` after initialization. It stops on the live bank-control screen until Enter, captures while the beep command remains active, then calls the matching ROM stop routine at `0x80061A4E`. This is a diagnostic mode: its purpose is to establish whether these addresses become readable while the DSP is actively processing the known-working beep.

Download a `rrdmp-ja` ZIP from [Actions](https://github.com/TheRainbowPhoenix/RRDMP/actions/workflows/build.yml), extract `RRDMP/` beside your libexword executable, and use `dict install RRDMP` after authenticating. No `dict reset` or SD formatting.

Source based on [original EX-word skeleton](https://github.com/TheRainbowPhoenix/exword-xd-a8500-TEST) and the known-working NORSD syscall flow. GPL-2.0 licensed; see COPYING.
