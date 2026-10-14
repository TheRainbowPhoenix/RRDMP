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

static const char * const paths16[] = {
    "\\\\crd0\\RAM16A.BIN", "\\\\crd0\\RAM16B.BIN"
};
static const char * const paths32[] = {
    "\\\\crd0\\ROM32A.BIN", "\\\\crd0\\ROM32B.BIN"
};
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
    if (key_state(KEY_BACK) || key_state(KEY_POWER)) current |= 4u;
    pressed = current & ~previous;
    previous = current;
    if (pressed & 4u) return -1;
    if (pressed & 1u) return 32;
    if (pressed & 2u) return 16;
    if (pressed & 8u) return 1;
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

static int dump_to_sd(uint32_t size)
{
    const char *outname = 0;
    const char * const *names = size == SIZE_32 ? paths32 : paths16;
    uint32_t base_address = size == SIZE_32 ? NOR_BASE : RAM_BASE;
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
        screen("CANNOT CREATE OUTPUT", "NOR16/32 A and B files may exist.",
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
    screen(size == SIZE_32 ? "DUMP ROM 80000000-81FFFFFF" : "DUMP RAM 8C000000-8CFFFFFF", "Writing exact selected mapped range.",
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
    int step;
    int choice = 0;
    unsigned last = 0;
    struct font *font = get_font();
    (void)font;

    screen("SELECT ROM32 OR RAM16 TO SD",
           "ENTER: ROM32 80000000-81FFFFFF",
           "PAGE DOWN: RAM16 8C000000-8CFFFFFF",
           "T: TINY SD FILE TEST / BACK: EXIT");

    for (;;) {
        volatile unsigned spin;
        step = poll_choice();
        if (step < 0) return -2;
        if (step != 0) {
            choice = step;
            if (choice == 1) {
                screen("SD FILE WRITE/READBACK TEST", "WRITES 16 BYTES TO CRD0",
                       "ENTER: CONFIRM TEST", "BACK: CANCEL");
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
        if (!key_state(KEY_ENTER) && !key_state(KEY_PAGE_DOWN)) break;
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
    else {
        step = dump_to_sd(choice == 32 ? SIZE_32 : SIZE_16);
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
