/*
 * RRDMP: 32 MiB ROM window or 16 MiB main RAM window to crd0.
 * Original devkitSH4 GCC + original libdataplus (hardware-tested toolchain).
 *
 * Based on the read source address and \\crd0\\ file syscalls demonstrated by
 * dictscript/dictscript's NOR_dumper; independent implementation.
 *
 * No CASIODICS check, because the target's first known header did not match.
 * Does not patch or erase firmware. Does write ONE new SD-card file.
 * The 16/32 MiB sizes are OPERATOR choices, not automatically detected sizes.
 */
#include <stdint.h>
#include <graphics/color.h>
#include <graphics/drawing.h>
#include <graphics/lcdc.h>
#include <graphics/text.h>
#include <sh4a/input/keypad.h>
#include <syscalls/syscalls.h>

#define NOR_BASE UINT32_C(0x80000000)
#define RAM_BASE UINT32_C(0x8C000000)
#define SIZE_16 UINT32_C(0x01000000)
#define SIZE_32 UINT32_C(0x02000000)
#define WRITE_CHUNK 4096u
#define HEADROOM UINT32_C(0x00100000)
#define FILE_VARIANTS 3u

/* Confirmed from ROM32A.BIN: initializes clocks, both DSP cores, their
 * firmware DMAs, completion handlers, and the I2C audio codec. */
typedef void (*rom_void_function)(void);
#define ROM_SPU2DSP_INITIALIZE ((rom_void_function)0x80061334u)
#define ROM_SPU2DSP1_START_BEEP ((rom_void_function)0x80061988u)
#define ROM_SPU2DSP1_STOP_BEEP  ((rom_void_function)0x80061A4Eu)

static const char * const paths16[] = {
    "\\\\crd0\\RAM16A.BIN", "\\\\crd0\\RAM16B.BIN", "\\\\crd0\\RAM16C.BIN"
};
static const char * const paths32[] = {
    "\\\\crd0\\ROM32A.BIN", "\\\\crd0\\ROM32B.BIN", "\\\\crd0\\ROM32C.BIN"
};
static const char digits[] = "0123456789ABCDEF";
static const char * const test_paths[] = {
    "\\\\crd0\\SDTSTA.TXT", "\\\\crd0\\SDTSTB.TXT", "\\\\crd0\\SDTSTC.TXT"
};
static const uint8_t test_bytes[16] = {
    'E','X','W','O','R','D','-','S','D','-','T','E','S','T','0','1'
};

/* Do not pass a DSP/MMIO address straight to sys_write().  The file syscall
 * changes execution context and can observe a different mapping (zeroes were
 * observed in the first capture).  This RAM buffer makes every source access
 * an explicit, volatile CPU bus read in this application before SD writing. */
static uint32_t dump_bounce_words[WRITE_CHUNK / sizeof(uint32_t)];

struct mapped_region {
    uint32_t address;
    uint32_t size;
    const char *label;
    const char * const *paths;
};

static const char * const dsp0_program_paths[] = {
    "\\\\crd0\\D0PRA.BIN", "\\\\crd0\\D0PRB.BIN", "\\\\crd0\\D0PRC.BIN"
};
static const char * const dsp0_data_paths[] = {
    "\\\\crd0\\D0DTA.BIN", "\\\\crd0\\D0DTB.BIN", "\\\\crd0\\D0DTC.BIN"
};
static const char * const dsp0_aux_paths[] = {
    "\\\\crd0\\D0AUXA.BIN", "\\\\crd0\\D0AUXB.BIN", "\\\\crd0\\D0AUXC.BIN"
};
static const char * const dsp0_register_paths[] = {
    "\\\\crd0\\D0REGA.BIN", "\\\\crd0\\D0REGB.BIN", "\\\\crd0\\D0REGC.BIN"
};
static const char * const dsp1_program_paths[] = {
    "\\\\crd0\\D1PRA.BIN", "\\\\crd0\\D1PRB.BIN", "\\\\crd0\\D1PRC.BIN"
};
static const char * const dsp1_data_paths[] = {
    "\\\\crd0\\D1DTA.BIN", "\\\\crd0\\D1DTB.BIN", "\\\\crd0\\D1DTC.BIN"
};
static const char * const dsp1_aux_paths[] = {
    "\\\\crd0\\D1AUXA.BIN", "\\\\crd0\\D1AUXB.BIN", "\\\\crd0\\D1AUXC.BIN"
};
static const char * const dsp1_register_paths[] = {
    "\\\\crd0\\D1REGA.BIN", "\\\\crd0\\D1REGB.BIN", "\\\\crd0\\D1REGC.BIN"
};
static const char * const dsp_mailbox_paths[] = {
    "\\\\crd0\\S2MBXA.BIN", "\\\\crd0\\S2MBXB.BIN", "\\\\crd0\\S2MBXC.BIN"
};
static const char * const spu2_register_report_paths[] = {
    "\\\\crd0\\SPU2RGA.TXT", "\\\\crd0\\SPU2RGB.TXT", "\\\\crd0\\SPU2RGC.TXT"
};

/* The raw 4 KiB D0REG file below contains this shared SPU2 control page.
 * This small report gives the important words names and preserves the exact
 * post-initialization values needed to decode the DSP0 PRAM/XRAM bank sizes. */
struct spu2_register {
    const char *name;
    uint32_t address;
};

static const struct spu2_register spu2_registers[] = {
    { "PBANKC0", UINT32_C(0xFE2FFC00) },
    { "PBANKC1", UINT32_C(0xFE2FFC04) },
    { "XBANKC0", UINT32_C(0xFE2FFC10) },
    { "XBANKC1", UINT32_C(0xFE2FFC14) },
    { "SPUSRST", UINT32_C(0xFE2FFC24) },
    { "SPUADR",  UINT32_C(0xFE2FFC28) },
    { "ENDIAN",  UINT32_C(0xFE2FFC2C) },
    { "GCOM0",   UINT32_C(0xFE2FFC40) },
    { "GCOM1",   UINT32_C(0xFE2FFC44) },
    { "GCOM2",   UINT32_C(0xFE2FFC48) },
    { "GCOM3",   UINT32_C(0xFE2FFC4C) },
    { "GCOM4",   UINT32_C(0xFE2FFC50) },
    { "GCOM5",   UINT32_C(0xFE2FFC54) },
    { "GCOM6",   UINT32_C(0xFE2FFC58) },
    { "GCOM7",   UINT32_C(0xFE2FFC5C) },
    { "DMABUF0", UINT32_C(0xFE2FFC80) },
    { "DMABUF1", UINT32_C(0xFE2FFC84) },
    { "DMABUF2", UINT32_C(0xFE2FFC88) },
    { "DMABUF3", UINT32_C(0xFE2FFC8C) }
};

/* Do not merge these into one broad range: the holes between them have not
 * been mapped and may fault.  Address/size pairs are Ghidra memory blocks. */
static const struct mapped_region spu_snapshot_regions[] = {
    { UINT32_C(0xFE200000), UINT32_C(0x00028000), "DSP0 PROGRAM", dsp0_program_paths },
    { UINT32_C(0xFE240000), UINT32_C(0x00038000), "DSP0 DATA",    dsp0_data_paths },
    { UINT32_C(0xFE280000), UINT32_C(0x0000A000), "DSP0 AUX",     dsp0_aux_paths },
    { UINT32_C(0xFE2FF000), UINT32_C(0x00001000), "DSP0 REGS + SPU2", dsp0_register_paths },
    { UINT32_C(0xFE300000), UINT32_C(0x00028000), "DSP1 PROGRAM", dsp1_program_paths },
    { UINT32_C(0xFE340000), UINT32_C(0x00038000), "DSP1 DATA",    dsp1_data_paths },
    { UINT32_C(0xFE380000), UINT32_C(0x0000A000), "DSP1 AUX",     dsp1_aux_paths },
    { UINT32_C(0xFE3C0000), UINT32_C(0x00001000), "DSP1 REGS",    dsp1_register_paths },
    { UINT32_C(0xFE3FF000), UINT32_C(0x00001000), "DSP MAILBOX",  dsp_mailbox_paths }
};

/* Diagnostics visible on the device, not only in the return value.
 * errno-like codes are syscalls' signed integers cast to hex for display. */
enum {
    CHECK_OK=0, CHECK_OPEN=-1, CHECK_SIZE=-2, CHECK_READ=-3,
    CHECK_CONTENT=-4, CHECK_CLOSE=-5
};

static void print_at(unsigned x, unsigned y, const char *str)
{
    render_text((uint16_t)x, (uint16_t)y, (char *)str);
}

static void hex8(char out[9], uint32_t value)
{
    unsigned i;
    for (i = 0; i < 8u; ++i) {
        out[i] = digits[(value >> (28u - 4u * i)) & 15u];
    }
    out[8] = '\0';
}

static void screen(const char *line1, const char *line2,
                   const char *line3, const char *line4)
{
    set_pen(create_rgb16(255, 255, 255));
    draw_rect(0, 0, 528, 320);
    set_pen(create_rgb16(23, 47, 65));
    print_at(14, 9, "RRDMP / ROM32 + RAM16 TO SD");
    set_pen(create_rgb16(80, 98, 114));
    print_at(14, 30, "ORIGINAL DEVKITSH4 / NO FIRMWARE WRITES");
    set_pen(create_rgb16(190, 209, 219));
    draw_rect(12, 53, 504, 1);
    set_pen(create_rgb16(23, 47, 65));
    if (line1) print_at(14, 78, line1);
    if (line2) print_at(14, 108, line2);
    if (line3) print_at(14, 138, line3);
    if (line4) print_at(14, 168, line4);
    set_pen(create_rgb16(100, 111, 124));
    print_at(14, 300, "STOP ON ERROR. NO FLASH/FORMAT COMMANDS.");
    lcdc_copy_vram();
}

static uint8_t key_state(unsigned key)
{
    return get_key_state((uint8_t)key) != 0u;
}

/* Return a new key press, not every spin while a key is held.
 * PAGE DOWN selects the *experimental* 32 MiB variant. */
static int poll_choice(void)
{
    static unsigned previous = 0u;
    unsigned current = 0u;
    unsigned pressed;
    keypad_read();
    if (key_state(KEY_ENTER)) current |= 1u;
    if (key_state(KEY_PAGE_DOWN)) current |= 2u;
    if (key_state(KEY_CHAR_T)) current |= 8u;
    if (key_state(KEY_CHAR_S)) current |= 16u;
    if (key_state(KEY_CHAR_B)) current |= 32u;
    if (key_state(KEY_BACK) || key_state(KEY_POWER)) current |= 4u;
    pressed = current & ~previous;
    previous = current;
    if (pressed & 4u) return -1;
    if (pressed & 1u) return 32;
    if (pressed & 2u) return 16;
    if (pressed & 8u) return 1;
    if (pressed & 16u) return 2;
    if (pressed & 32u) return 3;
    return 0;
}

static int choose_existing_free_filename(const char * const names[FILE_VARIANTS],
                                          const char **picked)
{
    unsigned i;
    for (i = 0; i < FILE_VARIANTS; ++i) {
        int fd = sys_open(names[i], FILE_RD);
        if (fd >= 0) {
            sys_close(fd);
            continue; /* Preserve all previous dumps, even incomplete ones. */
        }
        if (sys_create(names[i], 1) == 0) {
            *picked = names[i];
            return 1;
        }
    }
    return 0;
}

static int write_text(int fd, const char *text)
{
    unsigned count = 0u;
    int result;
    while (text[count] != '\0') ++count;
    result = sys_write(fd, text, (int)count);
    return result == (int)count ? 0 : -1;
}

/* All chosen dump regions and chunks are word aligned.  Word accesses avoid
 * relying on byte-lane behavior of the DSP/SPU memory windows.  Keeping the
 * words in ordinary RAM preserves the SH CPU's native byte order on SD. */
static void read_mapped_words(uint32_t address, uint32_t *destination,
                              unsigned byte_count)
{
    unsigned offset;
    for (offset = 0u; offset < byte_count; offset += sizeof(uint32_t)) {
        destination[offset / sizeof(uint32_t)] =
            *(const volatile uint32_t *)(uintptr_t)(address + offset);
    }
}

static int dump_spu2_register_report(void)
{
    const char *outname = 0;
    unsigned i;
    int fd;

    if (!choose_existing_free_filename(spu2_register_report_paths, &outname)) {
        screen("CANNOT CREATE SPU2 REPORT", "SPU2RGA / SPU2RGB already used?",
               "No existing file was deleted.", "BACK OR POWER: EXIT");
        return -1;
    }
    fd = sys_open(outname, FILE_WR);
    if (fd < 0) {
        screen("SPU2 REPORT OPEN FAILED", outname,
               "Empty file may remain.", "BACK OR POWER: EXIT");
        return -2;
    }

    if (write_text(fd, "SPU2 post-ROM-initialization register snapshot\r\n") != 0 ||
        write_text(fd, "Raw shared page: D0REG?.BIN FE2FF000-FE2FFFFF\r\n") != 0 ||
        write_text(fd, "PBANKC/XBANKC select DSP0 PRAM/XRAM bank layout.\r\n\r\n") != 0) {
        sys_close(fd);
        return -3;
    }
    for (i = 0u; i < sizeof(spu2_registers) / sizeof(spu2_registers[0]); ++i) {
        const struct spu2_register *reg = &spu2_registers[i];
        char address[9], value[9];
        uint32_t live_value = *(const volatile uint32_t *)(uintptr_t)reg->address;
        hex8(address, reg->address);
        hex8(value, live_value);
        if (write_text(fd, reg->name) != 0 || write_text(fd, " 0x") != 0 ||
            write_text(fd, address) != 0 || write_text(fd, " = 0x") != 0 ||
            write_text(fd, value) != 0 || write_text(fd, "\r\n") != 0) {
            sys_close(fd);
            return -3;
        }
    }
    if (sys_close(fd) != 0) return -4;
    return 0;
}

/* Shown before capture, rather than only written to SPU2RG?.TXT.  These are
 * repeated live volatile reads and therefore distinguish an SD-file issue
 * from an address-mapping/read-permission issue immediately on the device. */
static int show_live_spu2_bank_controls(int beep_active)
{
    static const uint32_t bank_addresses[4] = {
        UINT32_C(0xFE2FFC00), UINT32_C(0xFE2FFC04),
        UINT32_C(0xFE2FFC10), UINT32_C(0xFE2FFC14)
    };
    static const char * const bank_names[4] = {
        "PBANKC0", "PBANKC1", "XBANKC0", "XBANKC1"
    };
    unsigned i;

    screen(beep_active ? "DSP BEEP ACTIVE: LIVE READBACK" : "SPU2 LIVE REGISTER READBACK",
           "PBANKC0 / PBANKC1 / XBANKC0 / XBANKC1",
           "ENTER: CAPTURE NOW", "BACK: CANCEL");
    for (i = 0u; i < 4u; ++i) {
        char value[9];
        uint32_t live_value = *(const volatile uint32_t *)(uintptr_t)bank_addresses[i];
        hex8(value, live_value);
        print_at(14, 196 + 23 * i, bank_names[i]);
        print_at(145, 196 + 23 * i, value);
    }
    lcdc_copy_vram();

    for (;;) {
        volatile unsigned spin;
        keypad_read();
        if (key_state(KEY_BACK) || key_state(KEY_POWER)) return -1;
        if (key_state(KEY_ENTER)) return 0;
        for (spin = 0u; spin < 10000u; ++spin) { }
    }
}


/* Confirm the device's own read syscall can reopen a newly written file.
 * A verified size/readback does not guarantee libexword sees the same
 * namespace, nor is it a cryptographic validation of the ROM image. */
static int verify_file(const char *path, uint32_t expected,
                       const uint8_t *prefix, unsigned prefix_size,
                       uint32_t *actual_out, int *detail_out)
{
    uint8_t buf[16];
    unsigned i;
    int fd = sys_open(path, FILE_RD);
    int actual, n, close_rc;

    *actual_out = 0u;
    *detail_out = fd;
    if (fd < 0) return CHECK_OPEN;
    actual = sys_get_filesize(fd);
    *detail_out = actual;
    if (actual >= 0) *actual_out = (uint32_t)actual;
    if (actual < 0 || (uint32_t)actual != expected) {
        sys_close(fd);
        return CHECK_SIZE;
    }

    if (prefix_size > sizeof(buf)) {
        sys_close(fd);
        return CHECK_READ;
    }
    n = sys_read(fd, buf, (int)prefix_size);
    *detail_out = n;
    if (n != (int)prefix_size) {
        sys_close(fd);
        return CHECK_READ;
    }
    for (i = 0u; i < prefix_size; ++i) {
        if (buf[i] != prefix[i]) {
            *detail_out = (int)i;
            sys_close(fd);
            return CHECK_CONTENT;
        }
    }
    close_rc = sys_close(fd);
    *detail_out = close_rc;
    return close_rc == 0 ? CHECK_OK : CHECK_CLOSE;
}

static void show_file_check(const char *path, int check,
                            uint32_t actual, int detail)
{
    char bytes[9], rc[9];
    hex8(bytes, actual);
    hex8(rc, (uint32_t)detail);
    if (check == CHECK_OK)
        screen("FILE EXISTS / SIZE + PREFIX MATCH", path,
               "SIZE HEX BYTES:", bytes);
    else if (check == CHECK_OPEN)
        screen("POST-WRITE REOPEN FAILED", path,
               "OPEN RETURN (HEX):", rc);
    else if (check == CHECK_SIZE)
        screen("POST-WRITE SIZE MISMATCH", path,
               "ACTUAL SIZE (HEX):", bytes);
    else if (check == CHECK_READ)
        screen("POST-WRITE READ FAILED", path,
               "READ RETURN (HEX):", rc);
    else if (check == CHECK_CONTENT)
        screen("POST-WRITE DATA MISMATCH", path,
               "BAD OFFSET (HEX):", rc);
    else
        screen("POST-WRITE CLOSE FAILED", path,
               "CLOSE RETURN (HEX):", rc);
}

static void run_sd_probe(void)
{
    const char *name = 0;
    unsigned long space = 0;
    int fd, count, close_rc, check = 0, detail = 0;
    uint32_t actual = 0u;

    if (sys_freediskspace("crd0", &space) != 0 || space < 4096ul) {
        screen("SD TEST: CRD0 NOT AVAILABLE", "No file created.",
               "Check card, mount and free space.", "BACK: EXIT");
        return;
    }
    if (!choose_existing_free_filename(test_paths, &name)) {
        screen("SD TEST: CANNOT CREATE FILE", "SDTSTA / SDTSTB already used?",
               "No existing file was deleted.", "BACK: EXIT");
        return;
    }
    fd = sys_open(name, FILE_WR);
    if (fd < 0) {
        screen("SD TEST: OPEN FOR WRITE FAILED", name,
               "Empty file may remain.", "BACK: EXIT");
        return;
    }
    count = sys_write(fd, test_bytes, (int)sizeof(test_bytes));
    close_rc = sys_close(fd);
    if (count != (int)sizeof(test_bytes) || close_rc != 0) {
        char val[9];
        hex8(val, (uint32_t)(count != (int)sizeof(test_bytes) ? count : close_rc));
        screen("SD TEST: WRITE OR CLOSE FAILED", name,
               "RETURN VALUE (HEX):", val);
        return;
    }
    check = verify_file(name, (uint32_t)sizeof(test_bytes),
                        test_bytes, (unsigned)sizeof(test_bytes),
                        &actual, &detail);
    show_file_check(name, check, actual, detail);
}

static int dump_to_sd(uint32_t base_address, uint32_t size,
                      const char * const names[FILE_VARIANTS], const char *heading)
{
    const char *outname = 0;
    unsigned long available = 0;
    uint32_t completed = 0;
    uint32_t next_progress = 0;
    int fd;
    int failure = 0;
    int check_detail = 0;
    uint32_t actual = 0u;
    uint8_t prefix[16];
    unsigned i;
    char cur[9], end[9];

    /* No SD card? or not enough free storage? Stop BEFORE creating a file.
     * API is documented by libdataplus and dictscript docs/interrupts.md. */
    if (sys_freediskspace("crd0", &available) != 0) {
        screen("CRD0 NOT AVAILABLE", "Insert compatible SD/TF card.",
               "No file created.", "BACK OR POWER: EXIT");
        return -1;
    }
    if (available < size || available - size < HEADROOM) {
        screen("NOT ENOUGH FREE CRD0 SPACE", "Need ROM size + 1 MiB extra.",
               "No file created.", "BACK OR POWER: EXIT");
        return -2;
    }

    if (!choose_existing_free_filename(names, &outname)) {
        screen("CANNOT CREATE OUTPUT", "A, B and C output files may exist.",
               "No existing file was deleted.", "BACK OR POWER: EXIT");
        return -3;
    }
    fd = sys_open(outname, FILE_WR);
    if (fd < 0) {
        screen("FILE CREATED BUT OPEN FAILED", "An empty output may remain.",
               "Check CRD0 after exit.", "BACK OR POWER: EXIT");
        return -4;
    }

    hex8(end, size);
    screen(heading, "Writing exact selected mapped range.",
           "Do not remove card or power off.", "BACK cancels after a chunk.");

    while (completed < size) {
        uint32_t chunk = size - completed;
        uint32_t written = 0;
        if (chunk > WRITE_CHUNK) chunk = WRITE_CHUNK;

        /* Snapshot this chunk in the caller's CPU context before the file
         * syscall.  This is deliberately not a direct MMIO source pointer. */
        read_mapped_words(base_address + completed, dump_bounce_words,
                          (unsigned)chunk);
        while (written < chunk) {
            const uint8_t *source = (const uint8_t *)dump_bounce_words + written;
            int result = sys_write(fd, source, (int)(chunk - written));
            if (result <= 0 || (uint32_t)result > chunk - written) {
                failure = 1;
                break;
            }
            written += (uint32_t)result;
        }
        if (failure) break;

        completed += chunk;
        if (completed >= next_progress) {
            hex8(cur, completed);
            screen("DUMP IN PROGRESS (HEX BYTES)",
                   "WRITTEN:", cur, end);
            next_progress = completed + UINT32_C(0x00040000);
        }
        keypad_read();
        if (key_state(KEY_BACK) || key_state(KEY_POWER)) {
            failure = 2; /* Retain partial file; never silently delete. */
            break;
        }
    }

    if (sys_close(fd) != 0) failure = 3;

    if (failure) {
        hex8(cur, completed);
        screen("STOPPED / PARTIAL FILE REMAINS", "Copied bytes in hex:",
               cur, "CHECK CRD0 BEFORE RESTART.");
        return -(10 + failure);
    }

    /* Read back by path only after closing the write handle. This will
     * reveal false positive "completed" reports where SD shows no file. */
    read_mapped_words(base_address, dump_bounce_words, (unsigned)sizeof(prefix));
    for (i = 0u; i < sizeof(prefix); ++i)
        prefix[i] = ((const uint8_t *)dump_bounce_words)[i];
    {
        int check = verify_file(outname, size, prefix, (unsigned)sizeof(prefix),
                                &actual, &check_detail);
        show_file_check(outname, check, actual, check_detail);
        return check == CHECK_OK ? 0 : -20 + check;
    }
}

/* This is intentionally post-initialization only.  The ROM initializer
 * waits for its firmware DMA uploads before returning, so the captured
 * windows represent a usable DSP/SPU state rather than reset garbage. */
static int dump_spu_snapshot(int capture_while_beeping)
{
    unsigned i;
    int result;

    screen("INITIALIZE SPU2/DSP", "ROM FUNCTION 80061334", "DO NOT POWER OFF", "DSP FIRMWARE + CODEC SETUP");
    ROM_SPU2DSP_INITIALIZE();

    if (capture_while_beeping) {
        /* This is exactly the ROM route that produces the known-working DSP
         * beep: codec/clock enable, DSP1 command descriptor and COM0 doorbell. */
        ROM_SPU2DSP1_START_BEEP();
    }
    result = show_live_spu2_bank_controls(capture_while_beeping);
    if (result != 0) {
        if (capture_while_beeping) ROM_SPU2DSP1_STOP_BEEP();
        return result;
    }

    screen("WRITE SPU2 REGISTER REPORT", "PBANKC / XBANKC + SHARED STATE", "POST-INITIALIZATION VALUES", "DO NOT POWER OFF");
    result = dump_spu2_register_report();
    if (result != 0) {
        if (capture_while_beeping) ROM_SPU2DSP1_STOP_BEEP();
        return result;
    }

    for (i = 0u; i < sizeof(spu_snapshot_regions) / sizeof(spu_snapshot_regions[0]); ++i) {
        const struct mapped_region *region = &spu_snapshot_regions[i];
        result = dump_to_sd(region->address, region->size, region->paths, region->label);
        if (result != 0) {
            if (capture_while_beeping) ROM_SPU2DSP1_STOP_BEEP();
            return result;
        }
    }
    if (capture_while_beeping) ROM_SPU2DSP1_STOP_BEEP();
    screen("SPU2/DSP SNAPSHOT COMPLETE", "1 REPORT + 9 MAPPED WINDOWS", "FILES START SPU2RG / D0 / D1", "BACK OR POWER: EXIT");
    return 0;
}

int main(void)
{
    int step;
    int choice = 0;
    unsigned last = 0;
    struct font *font = get_font();
    (void)font;

    screen("SELECT ROM32 OR RAM16 TO SD",
           "ENTER: ROM32 80000000-81FFFFFF",
           "PAGE DOWN: RAM16 8C000000-8CFFFFFF",
           "S: SPU2 SNAPSHOT / B: BEEP + SNAPSHOT / T: SD TEST");

    for (;;) {
        volatile unsigned spin;
        step = poll_choice();
        if (step < 0) return -2;
        if (step != 0) {
            choice = step;
            if (choice == 1) {
                screen("SD FILE WRITE/READBACK TEST", "WRITES 16 BYTES TO CRD0",
                       "ENTER: CONFIRM TEST", "BACK: CANCEL");
            } else if (choice == 2) {
                screen("CONFIRM SPU2/DSP SNAPSHOT", "CALLS ROM DSP INITIALIZER", "REPORT + 9 WINDOWS TO CRD0", "ENTER: WRITE / BACK: CANCEL");
            } else if (choice == 3) {
                screen("CONFIRM BEEP-LIVE SNAPSHOT", "STARTS ROM DSP BEEP FIRST", "READS WHILE BEEP IS ACTIVE", "ENTER: START / BACK: CANCEL");
            } else if (choice == 16)
                screen("CONFIRM RAM16 (16 MiB)", "8C000000 - 8CFFFFFF",
                       "ENTER: WRITE NEW FILE TO CRD0",
                       "BACK: CANCEL");
            else
                screen("CONFIRM ROM32 (32 MiB)", "80000000 - 81FFFFFF",
                       "MAPPING NOT VERIFIED END-TO-END!",
                       "ENTER: WRITE / BACK: CANCEL");
            break;
        }
        for (spin = 0u; spin < 10000u; ++spin) { }
    }

    /* Require key release between choosing size and confirming. */
    for (;;) {
        volatile unsigned spin;
        keypad_read();
        if (!key_state(KEY_ENTER) && !key_state(KEY_PAGE_DOWN) &&
            !key_state(KEY_CHAR_S) && !key_state(KEY_CHAR_B)) break;
        for (spin = 0u; spin < 10000u; ++spin) { }
    }
    for (;;) {
        volatile unsigned spin;
        keypad_read();
        if (key_state(KEY_BACK) || key_state(KEY_POWER)) return -2;
        if (key_state(KEY_ENTER)) break;
        for (spin = 0u; spin < 10000u; ++spin) { }
    }
    if (choice == 1)
        run_sd_probe();
    else if (choice == 2)
        step = dump_spu_snapshot(0);
    else if (choice == 3)
        step = dump_spu_snapshot(1);
    else {
        if (choice == 32)
            step = dump_to_sd(NOR_BASE, SIZE_32, paths32, "DUMP ROM 80000000-81FFFFFF");
        else
            step = dump_to_sd(RAM_BASE, SIZE_16, paths16, "DUMP RAM 8C000000-8CFFFFFF");
        (void)step;
    }

    /* Keep result on screen until exit; don't hide errors automatically. */
    for (;;) {
        volatile unsigned spin;
        keypad_read();
        if (key_state(KEY_BACK) || key_state(KEY_POWER)) return -2;
        ++last;
        for (spin = 0u; spin < 20000u; ++spin) { }
    }
}
