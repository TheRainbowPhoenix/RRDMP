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
#define RS_BASE UINT32_C(0xFD800000)
#define ADDIN_BASE UINT32_C(0x74000000)
#define XRAM_BASE UINT32_C(0xE5007000)
#define YRAM_BASE UINT32_C(0xE5017000)
#define IL_BASE UINT32_C(0xE5200000)
#define RS_LENGTH UINT32_C(0x00010000)
#define XY_LENGTH UINT32_C(0x00002000)
#define IL_LENGTH UINT32_C(0x00004000)
#define SIZE_16 UINT32_C(0x01000000)
#define SIZE_32 UINT32_C(0x02000000)
#define WRITE_CHUNK 4096u
#define HEADROOM UINT32_C(0x00100000)

static const char * const paths16[] = {
    "\\\\crd0\\RAM16A.BIN", "\\\\crd0\\RAM16B.BIN"
};
static const char * const paths32[] = {
    "\\\\crd0\\ROM32A.BIN", "\\\\crd0\\ROM32B.BIN"
};
static const char * const paths_rs[] = {
    "\\\\crd0\\RS64KA.BIN", "\\\\crd0\\RS64KB.BIN"
};
static const char * const paths_addin[] = {
    "\\\\crd0\\ADD16A.BIN", "\\\\crd0\\ADD16B.BIN"
};
static const char * const paths_xram[] = {
    "\\\\crd0\\XRAM8A.BIN", "\\\\crd0\\XRAM8B.BIN"
};
static const char * const paths_yram[] = {
    "\\\\crd0\\YRAM8A.BIN", "\\\\crd0\\YRAM8B.BIN"
};
static const char * const paths_il[] = {
    "\\\\crd0\\IL16KA.BIN", "\\\\crd0\\IL16KB.BIN"
};
/* Every range uses [base, base+length), with the table's end exclusive
 * for XRAM, YRAM and IL. 1st and final byte are thus unambiguous. */
struct DumpRange {
    uint32_t base;
    uint32_t length;
    const char *label;
    const char * const *paths;
};
static const struct DumpRange ranges[] = {
    { NOR_BASE, SIZE_32, "ROM32 80000000-81FFFFFF", paths32 },
    { RAM_BASE, SIZE_16, "RAM16 8C000000-8CFFFFFF", paths16 },
    { RS_BASE, RS_LENGTH, "RS  FD800000-FD80FFFF", paths_rs },
    { ADDIN_BASE, SIZE_16, "ADDIN 74000000-74FFFFFF", paths_addin },
    { XRAM_BASE, XY_LENGTH, "XRAM E5007000-E5008FFF", paths_xram },
    { YRAM_BASE, XY_LENGTH, "YRAM E5017000-E5018FFF", paths_yram },
    { IL_BASE, IL_LENGTH, "IL   E5200000-E5203FFF", paths_il }
};
enum { CHOICE_ROM=0, CHOICE_RAM=1, CHOICE_RS=2,
       CHOICE_XRAM=4, CHOICE_YRAM=5, CHOICE_IL=6,
       CHOICE_ADDIN=3, CHOICE_TEST=7 };
static const char digits[] = "0123456789ABCDEF";
static const char * const test_paths[] = {
    "\\\\crd0\\SDTSTA.TXT", "\\\\crd0\\SDTSTB.TXT"
};
static const uint8_t test_bytes[16] = {
    'E','X','W','O','R','D','-','S','D','-','T','E','S','T','0','1'
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
    print_at(14, 9, "RRDMP / 6 MEMORY REGIONS TO SD");
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

/* On-device selection. Every key is edge-triggered; once a dump is
 * selected, a second Enter confirmation is mandatory. */
static int poll_choice(void)
{
    static uint32_t previous = 0u;
    uint32_t current = 0u, pressed;
    keypad_read();
    if (key_state(KEY_ENTER)) current |= 1u << 0;
    if (key_state(KEY_PAGE_DOWN)) current |= 1u << 1;
    if (key_state(KEY_CHAR_R)) current |= 1u << 2;
    if (key_state(KEY_CHAR_A)) current |= 1u << 8;
    if (key_state(KEY_CHAR_X)) current |= 1u << 3;
    if (key_state(KEY_CHAR_Y)) current |= 1u << 4;
    if (key_state(KEY_CHAR_I)) current |= 1u << 5;
    if (key_state(KEY_CHAR_T)) current |= 1u << 6;
    if (key_state(KEY_BACK) || key_state(KEY_POWER)) current |= 1u << 7;
    pressed = current & ~previous;
    previous = current;
    if (pressed & (1u << 7)) return -1;
    if (pressed & (1u << 0)) return CHOICE_ROM + 1;
    if (pressed & (1u << 1)) return CHOICE_RAM + 1;
    if (pressed & (1u << 2)) return CHOICE_RS + 1;
    if (pressed & (1u << 8)) return CHOICE_ADDIN + 1;
    if (pressed & (1u << 3)) return CHOICE_XRAM + 1;
    if (pressed & (1u << 4)) return CHOICE_YRAM + 1;
    if (pressed & (1u << 5)) return CHOICE_IL + 1;
    if (pressed & (1u << 6)) return CHOICE_TEST + 1;
    return 0;
}

static int choose_existing_free_filename(const char * const names[2],
                                          const char **picked)
{
    unsigned i;
    for (i = 0; i < 2u; ++i) {
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

static int dump_to_sd(const struct DumpRange *range)
{
    const char *outname = 0;
    const char * const *names = range->paths;
    uint32_t base_address = range->base;
    uint32_t size = range->length;
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
        screen("CANNOT CREATE OUTPUT", "A/B output names may exist.",
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
    screen(range->label, "Writing selected memory region.",
           "Do not remove card or power off.", "BACK cancels after a chunk.");

    while (completed < size) {
        uint32_t chunk = size - completed;
        uint32_t written = 0;
        if (chunk > WRITE_CHUNK) chunk = WRITE_CHUNK;
        while (written < chunk) {
            /* Match dictscript: hand the mapped ROM pointer directly to
             * the OS file syscall, but in bounded 4-KiB segments.
             * This can still fault if any chosen ROM address is unmapped. */
            const void *source = (const void *)(uintptr_t)
                                 (base_address + completed + written);
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
    for (i = 0u; i < sizeof(prefix); ++i)
        prefix[i] = *(const volatile uint8_t *)(uintptr_t)(base_address + i);
    {
        int check = verify_file(outname, size, prefix, (unsigned)sizeof(prefix),
                                &actual, &check_detail);
        show_file_check(outname, check, actual, check_detail);
        return check == CHECK_OK ? 0 : -20 + check;
    }
}

int main(void)
{
    int choice = 0, step;
    unsigned selection_key;
    struct font *font = get_font();
    (void)font;

    screen("SELECT REGION TO DUMP ON CRD0",
           "ENTER:ROM32 PGDN:RAM16 A:ADDIN",
           "R:RS64K X:XRAM Y:YRAM I:IL T:TEST",
           "BACK:EXIT; ALL REGIONS OPT-IN");
    for (;;) {
        volatile unsigned spin;
        step = poll_choice();
        if (step < 0) return -2;
        if (step > 0) {
            choice = step - 1;
            if (choice == CHOICE_TEST)
                screen("CONFIRM 16-BYTE SD TEST", "No memory region is read.",
                       "ENTER:TEST  BACK:CANCEL",
                       "Creates new SDTSTA/B.TXT");
            else
                screen("CONFIRM MEMORY DUMP TO CRD0",
                       ranges[choice].label,
                       "INVALID/PRIVILEGED READ MAY REBOOT!",
                       "ENTER:WRITE  BACK:CANCEL");
            break;
        }
        for (spin=0;spin<10000u;spin++) { }
    }

    /* Wait for all selection keys to be released; prevent accidental
     * confirmation from the selection keystroke. */
    do {
        keypad_read();
        selection_key = key_state(KEY_ENTER) ||
                        key_state(KEY_PAGE_DOWN) ||
                        key_state(KEY_CHAR_R) ||
                        key_state(KEY_CHAR_A) ||
                        key_state(KEY_CHAR_X) ||
                        key_state(KEY_CHAR_Y) ||
                        key_state(KEY_CHAR_I) ||
                        key_state(KEY_CHAR_T);
    } while (selection_key);
    for (;;) {
        volatile unsigned spin;
        keypad_read();
        if (key_state(KEY_BACK) || key_state(KEY_POWER)) return -2;
        if (key_state(KEY_ENTER)) break;
        for (spin=0;spin<10000u;spin++) { }
    }

    if (choice == CHOICE_TEST) run_sd_probe();
    else {
        step = dump_to_sd(&ranges[choice]);
        (void)step;
    }
    for (;;) {
        volatile unsigned spin;
        keypad_read();
        if (key_state(KEY_BACK) || key_state(KEY_POWER)) return -2;
        for (spin=0;spin<20000u;spin++) { }
    }
}
