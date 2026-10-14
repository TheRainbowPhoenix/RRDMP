# RRDMP — EX-word ROM32 and RAM16 dumper

This add-on is built using the original devkitSH4 compiler and original precompiled libdataplus libraries, because that combination works on the target hardware.

| Selection | Memory | Starting address | Ending address | Bytes |
|---|---|---|---|---|
| Enter | ROM32 | `0x80000000` | `0x81FFFFFF` | 33,554,432 |
| Page Down | RAM16 | `0x8C000000` | `0x8CFFFFFF` | 16,777,216 |

After selecting, **release the key and press Enter again** to confirm. Back exits. Files are saved to the SD card at `\\crd0\` as `ROM32A.BIN` / `ROM32B.BIN` or `RAM16A.BIN` / `RAM16B.BIN`. Existing files are not deleted. A 16-byte SD filesystem probe is also available using T.

The code reads the selected ROM/RAM range and writes an ordinary file to SD; it does not deliberately write to firmware or RAM. The range may still include unmapped addresses and cause a reboot. Live RAM may change during capture and can contain private data. The image is **not validated on hardware** just because CI builds it.

Download a `rrdmp-ja` ZIP from [Actions](https://github.com/TheRainbowPhoenix/RRDMP/actions/workflows/build.yml), extract `RRDMP/` beside your libexword executable, and use `dict install RRDMP` after authenticating. No `dict reset` or SD formatting.

Source based on [original EX-word skeleton](https://github.com/TheRainbowPhoenix/exword-xd-a8500-TEST) and the known-working NORSD syscall flow. GPL-2.0 licensed; see COPYING.
