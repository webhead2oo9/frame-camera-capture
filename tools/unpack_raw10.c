// Unpack MIPI CSI-2 RAW10 (V4L2 'Y10P' / 'pgAA' etc.) into 16-bit little-endian samples.
//
// RAW10 packs 4 pixels into 5 bytes: bytes 0-3 hold bits 9..2 of pixels 0-3,
// byte 4 holds bits 1..0 of pixel 0 in bits 1..0, pixel 1 in bits 3..2, and so on.
// Rows may be padded: pass the V4L2 bytesperline as <stride>.
//
// Output samples are left-aligned (value << 6) so 16-bit consumers such as
// ffmpeg's gray16le / bayer_*16le see the full dynamic range. Use --no-shift to
// keep the native 0..1023 range instead.
//
// usage: unpack_raw10 <in.raw> <out.raw16> <width> <height> <stride> [--no-shift]
#define _POSIX_C_SOURCE 200809L
#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

static size_t dimension(const char *s) {
    char *end;
    errno = 0;
    uintmax_t value = strtoumax(s, &end, 10);
    if (*s < '0' || *s > '9' || *end || errno || !value || value > SIZE_MAX) {
        fprintf(stderr, "invalid dimension: %s\n", s);
        exit(2);
    }
    return (size_t)value;
}

int main(int argc, char **argv) {
    if (argc != 6 && argc != 7) {
        fprintf(stderr, "usage: %s <in.raw> <out.raw16> <width> <height> <stride> [--no-shift]\n", argv[0]);
        return 2;
    }
    const size_t width = dimension(argv[3]);
    const size_t height = dimension(argv[4]);
    const size_t stride = dimension(argv[5]);
    const int shift = (argc == 7 && strcmp(argv[6], "--no-shift") == 0) ? 0 : 6;
    if (argc == 7 && shift != 0) {
        fprintf(stderr, "unknown option: %s\n", argv[6]);
        return 2;
    }
    if (width % 4 != 0 || width > SIZE_MAX / 2 || stride < width / 4 * 5) {
        fprintf(stderr, "invalid geometry: width must be a multiple of 4, width*2 must fit, stride >= width/4*5\n");
        return 2;
    }

    FILE *in = fopen(argv[1], "rb");
    if (!in) { perror(argv[1]); return 1; }
    // Compare opened files before truncating; path comparisons miss hard links.
    int out_fd = open(argv[2], O_WRONLY | O_CREAT, 0666);
    if (out_fd < 0) { perror(argv[2]); fclose(in); return 1; }
    struct stat in_stat, out_stat;
    if (fstat(fileno(in), &in_stat) || fstat(out_fd, &out_stat)) {
        perror("stat input/output");
        close(out_fd);
        fclose(in);
        return 1;
    }
    if (in_stat.st_dev == out_stat.st_dev && in_stat.st_ino == out_stat.st_ino) {
        fputs("input and output refer to the same file\n", stderr);
        close(out_fd);
        fclose(in);
        return 1;
    }
    if (S_ISREG(out_stat.st_mode) && ftruncate(out_fd, 0)) {
        perror(argv[2]);
        close(out_fd);
        fclose(in);
        return 1;
    }
    FILE *out = fdopen(out_fd, "wb");
    if (!out) { perror(argv[2]); close(out_fd); fclose(in); return 1; }

    uint8_t *row = malloc(stride);
    uint8_t *dst = malloc(width * 2);
    if (!row || !dst) {
        fputs("out of memory\n", stderr);
        free(row);
        free(dst);
        fclose(in);
        fclose(out);
        return 1;
    }

    int rc = 0;
    for (size_t y = 0; y < height; ++y) {
        if (fread(row, 1, stride, in) != stride) {
            fprintf(stderr, "short read at row %zu (expected %zu rows of %zu bytes)\n", y, height, stride);
            rc = 1;
            break;
        }
        const uint8_t *s = row;
        uint8_t *d = dst;
        for (size_t x = 0; x < width; x += 4, s += 5) {
            const uint8_t lsb = s[4];
            for (int i = 0; i < 4; ++i) {
                const uint16_t v = (uint16_t)(((uint16_t)s[i] << 2) | ((lsb >> (2 * i)) & 3u)) << shift;
                *d++ = (uint8_t)(v & 0xff);
                *d++ = (uint8_t)(v >> 8);
            }
        }
        if (fwrite(dst, 1, width * 2, out) != width * 2) {
            perror(argv[2]);
            rc = 1;
            break;
        }
    }
    free(row);
    free(dst);
    fclose(in);
    if (fclose(out) != 0) { perror(argv[2]); rc = 1; }
    return rc;
}
