# RRDMP — EX-word ROM32 and RAM16 dumper

This add-on is built using the original devkitSH4 compiler and original precompiled libdataplus libraries, because that combination works on the target hardware.

| Selection | Memory | Starting address | Ending address | Bytes |
|---|---|---|---|---|
| Enter | ROM32 | `0x80000000` | `0x81FFFFFF` | 33,554,432 |
| Page Down | RAM16 | `0x8C000000` | `0x8CFFFFFF` | 16,777,216 |

After selecting, **release the key and press Enter again** to confirm. Back exits. Files are saved to the SD card at `\\crd0\` as `ROM32A.BIN` / `ROM32B.BIN` or `RAM16A.BIN` / `RAM16B.BIN`. Existing files are not deleted. A 16-byte SD filesystem probe is also available using T.

The code reads the selected ROM/RAM range and writes an ordinary file to SD; it does not deliberately write to firmware or RAM. The range may still include unmapped addresses and cause a reboot. Live RAM may change during capture and can contain private data. The image is **not validated on hardware** just because CI builds it.

## Experimental extra memory dumps (on-chip)

The `experiment/extra-onchip-memory` branch adds four additional,
**independently confirmed, opt-in** dump choices to the original ROM32 and
RAM16 menu. These high-address reads may fault or be inaccessible from
the ordinary EX-word add-on privilege level; compilation proves nothing
about their runtime accessibility.

| Key | Area | Inclusive first/last address | Bytes | SD filename |
| --- | --- | --- | ---: | --- |
| Enter | ROM32 | `0x80000000–0x81FFFFFF` | 33,554,432 | `ROM32A.BIN` |
| Page Down | RAM16 | `0x8C000000–0x8CFFFFFF` | 16,777,216 | `RAM16A.BIN` |
| **R** | **RS** | `0xFD800000–0xFD8007FF` | **2,048** | `RS2KA.BIN` |
| **X** | **XRAM** | `0xE5007000–0xE5008FFF` | **8,192** | `XRAM8A.BIN` |
| **Y** | **YRAM** | `0xE5017000–0xE5018FFF` | **8,192** | `YRAM8A.BIN` |
| **I** | **IL** | `0xE5200000–0xE5203FFF` | **16,384** | `IL16KA.BIN` |
| **T** | SD test | — | 16 | `SDTSTA.TXT` |

Each option requires a **second Enter confirmation** after releasing
the selection key. A second `B` filename is used if the corresponding
`A` filename already exists; neither is overwritten.

**Address convention:** the originally supplied XRAM, YRAM and IL
addresses end at `0xE5009000`, `0xE5019000`, and `0xE5204000`,
respectively. Those addresses are **exclusive bounds**, consistent with
the sizes of 8 KiB, 8 KiB and 16 KiB. Dumped last bytes are the
preceding addresses listed in the table.

**Significant risk:** RS at `0xFD800000`, XRAM/YRAM around `0xE5000000`,
and IL at `0xE5200000` may be physical/MMIO or privileged mappings
not accessible to an ordinary user-mode process. Any read may reboot or
hang the EX-word, including the first byte. Do not test them on a device
you cannot recover. Reading hardware registers can also have side
effects. The app does not write through these source addresses but does
create a file on SD.

Like the existing app, the dump syscall receives a source pointer
directly and outputs chunks of at most 4 KiB to `\\crd0\`. It
verifies the resulting file's size and first 16 bytes when available.
RAM contents may change while dumping, potentially causing a prefix
comparison mismatch despite a complete file. Dumps may contain private
session or application data.

Download a `rrdmp-ja` ZIP from [Actions](https://github.com/TheRainbowPhoenix/RRDMP/actions/workflows/build.yml), extract `RRDMP/` beside your libexword executable, and use `dict install RRDMP` after authenticating. No `dict reset` or SD formatting.

Source based on [original EX-word skeleton](https://github.com/TheRainbowPhoenix/exword-xd-a8500-TEST) and the known-working NORSD syscall flow. GPL-2.0 licensed; see COPYING.
