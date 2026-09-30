// Experimental Steam Frame camera tap. See ../README.md for layout and race limits.
// Never opens camera devices or controls XRService. Only DMA_BUF_IOCTL_SYNC(READ).
#define _GNU_SOURCE
#include <dirent.h>
#include <endian.h>
#include <errno.h>
#include <fcntl.h>
#include <getopt.h>
#include <grp.h>
#include <inttypes.h>
#include <limits.h>
#include <linux/dma-buf.h>
#include <poll.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <time.h>
#include <unistd.h>

#define RING 16
#define PAIR (2 * RING)
#define KIND_COUNT 3
#define ISP "acb8000.isp"

struct buffer {
    uint64_t ino;
    size_t size;
    int source_fd, fd;
    void *map;
};
struct kind {
    const char *name;
    unsigned width, height, stride;
    size_t image_size, meta_size;
    struct buffer image[PAIR], meta[PAIR];
    unsigned ni, nm;
};
static struct kind kinds[KIND_COUNT] = {
    {.name = "arcturus", .width = 2464, .height = 2464, .stride = 3328,
     .image_size = 12353536, .meta_size = 1064960},
    {.name = "slam", .width = 1056, .height = 1024, .stride = 1056,
     .image_size = 1769472, .meta_size = 151552},
    {.name = "upper", .width = 640, .height = 480, .stride = 640,
     .image_size = 462848, .meta_size = 8192},
};
struct camera {
    struct kind *kind;
    char name[32];
    unsigned base, saved;
    uint64_t last, deadline;
};
static int target_pidfd = -1;

static void fail(const char *message) {
    fprintf(stderr, "frametap: %s\n", message);
    exit(1);
}
static void die(const char *operation) {
    fprintf(stderr, "frametap: %s: %s\n", operation, strerror(errno));
    exit(1);
}
static unsigned number(const char *s, unsigned min) {
    char *end;
    errno = 0;
    unsigned long v = strtoul(s, &end, 10);
    if (!*s || *s < '0' || *s > '9' || *end || errno || v < min || v > INT_MAX)
        fail("invalid numeric argument");
    return (unsigned)v;
}
static bool numeric(const char *s) {
    return *s && strspn(s, "0123456789") == strlen(s);
}
static uint64_t now_ns(void) {
    struct timespec ts;
    if (clock_gettime(CLOCK_MONOTONIC, &ts)) die("clock_gettime");
    return (uint64_t)ts.tv_sec * 1000000000 + (uint64_t)ts.tv_nsec;
}
static void alive(void) {
    struct pollfd p = {.fd = target_pidfd, .events = POLLIN};
    int rc;
    do { rc = poll(&p, 1, 0); } while (rc < 0 && errno == EINTR);
    if (rc < 0) die("poll(pidfd)");
    if (rc) fail("XRService exited; discard this session and rediscover buffers");
}
static bool is_xrservice(unsigned pid) {
    char path[64], exe[PATH_MAX];
    snprintf(path, sizeof(path), "/proc/%u/exe", pid);
    ssize_t n = readlink(path, exe, sizeof(exe) - 1);
    if (n < 0 || (size_t)n >= sizeof(exe) - 1) return false;
    exe[n] = 0;
    const char *base = strrchr(exe, '/');
    return base && !strcmp(base + 1, "XRService");
}
static unsigned find_pid(void) {
    DIR *d = opendir("/proc");
    if (!d) die("opendir /proc");
    unsigned pid = 0;
    struct dirent *e;
    while ((e = readdir(d))) {
        if (!numeric(e->d_name)) continue;
        unsigned candidate = number(e->d_name, 1);
        if (!is_xrservice(candidate)) continue;
        if (pid) fail("multiple XRService processes; select one with --pid");
        pid = candidate;
    }
    closedir(d);
    if (!pid) fail("no XRService found; this tool does not start VR");
    return pid;
}
static bool known_size(size_t n) {
    for (unsigned k = 0; k < KIND_COUNT; ++k)
        if (n == kinds[k].image_size || n == kinds[k].meta_size) return true;
    return false;
}

// Debugfs separates objects by header rows, not by an ABI-stable binary layout.
static size_t isp_inodes(uint64_t *inodes, size_t capacity) {
    FILE *f = fopen("/sys/kernel/debug/dma_buf/bufinfo", "r");
    if (!f) die("read DMA bufinfo (run via sudo; debugfs must already be mounted)");
    char *line = NULL;
    size_t len = 0, count = 0, size;
    uint64_t ino = 0, next;
    char exporter[64], device[128];
    bool candidate = false, devices = false;
    while (getline(&line, &len, f) >= 0) {
        if (sscanf(line, "%zu %*x %*x %*u %63s %" SCNu64,
                   &size, exporter, &next) == 3) {
            ino = next;
            candidate = !strcmp(exporter, "udmabuf") && known_size(size);
            devices = false;
        } else if (strstr(line, "Attached Devices:")) {
            devices = true;
        } else if (candidate && devices && sscanf(line, "%127s", device) == 1 &&
                   !strcmp(device, ISP)) {
            if (count == capacity) fail("too many ISP buffers for this firmware profile");
            inodes[count++] = ino;
            candidate = false;
        }
    }
    if (ferror(f)) die("read DMA bufinfo");
    free(line);
    fclose(f);
    return count;
}
static bool fd_info(const char *path, struct buffer *b) {
    FILE *f = fopen(path, "r");
    if (!f) {
        if (errno == ENOENT) return false;
        die("read fdinfo");
    }
    char line[256], exporter[64] = "";
    b->ino = 0;
    b->size = 0;
    while (fgets(line, sizeof(line), f)) {
        if (sscanf(line, "ino: %" SCNu64, &b->ino) == 1) continue;
        if (sscanf(line, "size: %zu", &b->size) == 1) continue;
        (void)sscanf(line, "exp_name: %63s", exporter);
    }
    if (ferror(f)) die("read fdinfo");
    fclose(f);
    return b->ino && b->size && !strcmp(exporter, "udmabuf");
}
static void append_buffer(struct buffer *a, unsigned *count, struct buffer b) {
    for (unsigned i = 0; i < *count; ++i) if (a[i].ino == b.ino) return;
    if (*count == PAIR) fail("unexpected ring size; refusing to guess camera layout");
    a[(*count)++] = b;
}
static int inode_order(const void *a, const void *b) {
    uint64_t x = ((const struct buffer *)a)->ino, y = ((const struct buffer *)b)->ino;
    return (x > y) - (x < y);
}
static void discover(unsigned pid) {
    uint64_t inodes[KIND_COUNT * PAIR * 2];
    size_t count = isp_inodes(inodes, sizeof(inodes) / sizeof(*inodes));
    char directory[64], path[128];
    snprintf(directory, sizeof(directory), "/proc/%u/fdinfo", pid);
    DIR *d = opendir(directory);
    if (!d) die("opendir XRService fdinfo");
    struct dirent *e;
    while ((e = readdir(d))) {
        if (!numeric(e->d_name)) continue;
        unsigned fd = number(e->d_name, 0);
        snprintf(path, sizeof(path), "%s/%u", directory, fd);
        struct buffer b = {.source_fd = (int)fd, .fd = -1};
        if (!fd_info(path, &b)) continue;
        bool attached = false;
        for (size_t i = 0; i < count; ++i) if (inodes[i] == b.ino) attached = true;
        if (!attached) continue;
        for (unsigned k = 0; k < KIND_COUNT; ++k) {
            struct kind *kind = &kinds[k];
            if (b.size == kind->image_size) append_buffer(kind->image, &kind->ni, b);
            if (b.size == kind->meta_size) append_buffer(kind->meta, &kind->nm, b);
        }
    }
    closedir(d);
    for (unsigned k = 0; k < KIND_COUNT; ++k) {
        struct kind *kind = &kinds[k];
        if (!kind->ni && !kind->nm) continue;
        if (kind->ni != PAIR || kind->nm != PAIR) {
            fprintf(stderr, "%s: %u images, %u metadata buffers (expected 32 each)\n",
                    kind->name, kind->ni, kind->nm);
            fail("unsupported or changing allocation layout; no capture attempted");
        }
        qsort(kind->image, PAIR, sizeof(struct buffer), inode_order);
        qsort(kind->meta, PAIR, sizeof(struct buffer), inode_order);
        // Observed allocation pattern is image, metadata, image, metadata.
        // This rejects interleaving but is NOT proof of V4L2 plane association.
        for (unsigned i = 0; i < PAIR; ++i) {
            if (kind->image[i].ino >= kind->meta[i].ino ||
                (i + 1 < PAIR && kind->meta[i].ino >= kind->image[i + 1].ino))
                fail("allocation ordering changed; cannot infer image/metadata pairs");
        }
    }
    alive();
}
static void map_buffer(struct buffer *b) {
    b->fd = (int)syscall(SYS_pidfd_getfd, target_pidfd, b->source_fd, 0);
    if (b->fd < 0) die("pidfd_getfd (requires ptrace permission; normally sudo)");
    char path[64];
    snprintf(path, sizeof(path), "/proc/self/fdinfo/%d", b->fd);
    struct buffer current = {0};
    if (!fd_info(path, &current) || current.ino != b->ino || current.size != b->size)
        fail("source fd changed during discovery; rerun after camera setup finishes");
    b->map = mmap(NULL, b->size, PROT_READ, MAP_SHARED, b->fd, 0);
    if (b->map == MAP_FAILED) die("mmap(PROT_READ)");
}
static void drop_root(void) {
    if (geteuid() != 0) return;
    const char *uid = getenv("SUDO_UID"), *gid = getenv("SUDO_GID");
    if (!uid && !gid) {
        fprintf(stderr, "Warning: direct root invocation; output remains root-owned. Prefer sudo.\n");
        return;
    }
    if (!uid || !gid) fail("incomplete sudo identity");
    unsigned u = number(uid, 0), g = number(gid, 0);
    if (setgroups(0, NULL) || setresgid(g, g, g) || setresuid(u, u, u)) die("drop privileges");
}
static void sync_read(struct buffer *b, uint64_t phase) {
    struct dma_buf_sync s = {.flags = DMA_BUF_SYNC_READ | phase};
    int rc;
    do { rc = ioctl(b->fd, DMA_BUF_IOCTL_SYNC, &s); }
    while (rc < 0 && (errno == EINTR || errno == EAGAIN));
    if (rc) die("DMA_BUF_IOCTL_SYNC(READ)");
}
static uint64_t marker(struct buffer *b) {
    sync_read(b, DMA_BUF_SYNC_START);
    uint64_t v = le64toh(*(const volatile uint64_t *)b->map);
    sync_read(b, DMA_BUF_SYNC_END);
    return v;
}
static unsigned select_slot(struct camera *c, uint64_t *second, uint64_t *newest) {
    unsigned slot = c->base, best_slot = c->base;
    *second = *newest = 0;
    for (unsigned i = c->base; i < c->base + RING; ++i) {
        uint64_t v = marker(&c->kind->meta[i]);
        if (v > *newest) {
            *second = *newest;
            slot = best_slot;
            *newest = v;
            best_slot = i;
        } else if (v < *newest && v > *second) {
            *second = v;
            slot = i;
        }
    }
    return slot;
}
static void write_all(int fd, const void *data, size_t size) {
    const unsigned char *p = data;
    while (size) {
        ssize_t n = write(fd, p, size);
        if (n < 0 && errno == EINTR) continue;
        if (n <= 0) die("write frame");
        p += n;
        size -= (size_t)n;
    }
}
static void save_bytes(int directory, const char *name, const char *header,
                       const void *data, size_t size) {
    int fd = openat(directory, name, O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC | O_NOFOLLOW, 0600);
    if (fd < 0) die("create capture file");
    if (header) write_all(fd, header, strlen(header));
    write_all(fd, data, size);
    if (close(fd)) die("close capture file");
}
static void publish(int directory, const char *part) {
    char name[128];
    size_t n = strlen(part);
    if (n < 5 || n >= sizeof(name)) fail("invalid internal filename");
    memcpy(name, part, n - 5);
    name[n - 5] = 0;
    if (renameat(directory, part, directory, name)) die("publish capture file");
}
static bool capture(struct camera *c, unsigned slot, uint64_t value,
                    int directory, unsigned pid, bool full) {
    struct kind *k = c->kind;
    struct buffer *image = &k->image[slot], *meta = &k->meta[slot];
    if (marker(meta) != value) return false;
    bool color = k == &kinds[0];
    size_t bytes = (size_t)k->stride * k->height * (color ? 3 : 2) / 2;
    char file[128], image_file[128], meta_file[128], json_file[128], pgm[64];
    snprintf(file, sizeof(file), "%s-%06u.%s.part", c->name, c->saved + 1, color ? "nv12" : "pgm");
    snprintf(image_file, sizeof(image_file), "%s-%06u.image.bin.part", c->name, c->saved + 1);
    snprintf(meta_file, sizeof(meta_file), "%s-%06u.metadata.bin.part", c->name, c->saved + 1);
    snprintf(json_file, sizeof(json_file), "%s-%06u.json.part", c->name, c->saved + 1);
    snprintf(pgm, sizeof(pgm), "P5\n%u %u\n255\n", k->width, k->height);
    uint64_t begin = now_ns();
    sync_read(image, DMA_BUF_SYNC_START);
    save_bytes(directory, file, color ? NULL : pgm, image->map, bytes);
    if (full) save_bytes(directory, image_file, NULL, image->map, image->size);
    sync_read(image, DMA_BUF_SYNC_END);
    if (full) {
        sync_read(meta, DMA_BUF_SYNC_START);
        save_bytes(directory, meta_file, NULL, meta->map, meta->size);
        sync_read(meta, DMA_BUF_SYNC_END);
    }
    uint64_t after = marker(meta), end = now_ns();
    alive();
    if (after != value) {
        if (unlinkat(directory, file, 0)) die("remove overwritten capture");
        if (full && (unlinkat(directory, image_file, 0) || unlinkat(directory, meta_file, 0)))
            die("remove overwritten full buffers");
        return false;
    }
    char json[2048];
    int n = snprintf(json, sizeof(json),
        "{\n  \"camera\": \"%s\", \"pid\": %u,\n"
        "  \"format\": \"%s\", \"width\": %u, \"height\": %u, \"stride\": %u,\n"
        "  \"image_payload_bytes\": %zu, \"uv_offset\": %zu,\n"
        "  \"image_fd\": %d, \"metadata_fd\": %d,\n"
        "  \"image_inode\": %" PRIu64 ", \"metadata_inode\": %" PRIu64 ",\n"
        "  \"image_allocation_bytes\": %zu, \"metadata_allocation_bytes\": %zu,\n"
        "  \"metadata_first_u64\": \"%" PRIu64 "\", \"marker_units\": \"unknown\",\n"
        "  \"copy_begin_monotonic_ns\": %" PRIu64 ", \"copy_end_monotonic_ns\": %" PRIu64 ",\n"
        "  \"association\": \"inferred-allocation-order\", \"tear_free_guaranteed\": false,\n"
        "  \"full_buffers\": %s\n}\n",
        c->name, pid, color ? "nv12" : "gray8-pgm", k->width, k->height, k->stride,
        bytes, color ? (size_t)k->stride * k->height : 0, image->source_fd, meta->source_fd,
        image->ino, meta->ino, image->size, meta->size, value, begin, end, full ? "true" : "false");
    if (n < 0 || (size_t)n >= sizeof(json)) fail("capture metadata overflow");
    save_bytes(directory, json_file, NULL, json, (size_t)n);
    publish(directory, file);
    if (full) { publish(directory, image_file); publish(directory, meta_file); }
    // JSON published last is the completion record; .part files are never valid frames.
    publish(directory, json_file);
    ++c->saved;
    c->last = value;
    c->deadline = end + 5000000000ULL;
    printf("%s frame=%u marker=%" PRIu64 " bytes=%zu\n", c->name, c->saved, value, bytes);
    return true;
}
static void usage(void) {
    puts("Usage: frametap [--pid PID] [--camera NAME] [--count N] [--output NEW_DIR] [--full] [--list]\n"
         "Camera: all (default), arcturus-a, arcturus-b, slam-a, slam-b, upper-a, upper-b\n"
         "Default: one newly observed frame per available camera, under captures/tap-TIME-PID.\n"
         "--full also saves entire image and metadata allocations. --list writes no files.\n"
         "Requires sudo for debugfs and pidfd_getfd; drops to the sudo user before output.\n"
         "Experimental: inferred camera/plane association, unknown marker clock, no DMA fence.\n"
         "Never controls camera devices, starts/stops services, or activates OpenVR cameras.");
}
int main(int argc, char **argv) {
    unsigned pid = 0, count = 1;
    const char *selected = "all", *output = NULL;
    bool list = false, full = false;
    static const struct option options[] = {
        {"pid", required_argument, NULL, 'p'}, {"camera", required_argument, NULL, 'c'},
        {"count", required_argument, NULL, 'n'}, {"output", required_argument, NULL, 'o'},
        {"full", no_argument, NULL, 'f'}, {"list", no_argument, NULL, 'l'},
        {"help", no_argument, NULL, 'h'}, {NULL, 0, NULL, 0}
    };
    int opt;
    while ((opt = getopt_long(argc, argv, "p:c:n:o:flh", options, NULL)) != -1) {
        switch (opt) {
            case 'p': pid = number(optarg, 1); break;
            case 'c': selected = optarg; break;
            case 'n': count = number(optarg, 1); break;
            case 'o': output = optarg; break;
            case 'f': full = true; break;
            case 'l': list = true; break;
            case 'h': usage(); return 0;
            default: usage(); return 2;
        }
    }
    if (optind != argc) { usage(); return 2; }
    bool all = !strcmp(selected, "all"), valid = all;
    for (unsigned k = 0; k < KIND_COUNT; ++k) for (unsigned half = 0; half < 2; ++half) {
        char name[32];
        snprintf(name, sizeof(name), "%s-%c", kinds[k].name, 'a' + half);
        if (!strcmp(selected, name)) valid = true;
    }
    if (!valid) fail("unknown camera; see --help");
    if (!pid) pid = find_pid();
    target_pidfd = (int)syscall(SYS_pidfd_open, pid, 0);
    if (target_pidfd < 0) die("pidfd_open");
    if (!is_xrservice(pid)) fail("target executable is not XRService");
    alive();
    discover(pid);
    struct camera cameras[2 * KIND_COUNT] = {0};
    unsigned nc = 0;
    for (unsigned k = 0; k < KIND_COUNT; ++k) for (unsigned half = 0; half < 2; ++half) {
        char name[32];
        snprintf(name, sizeof(name), "%s-%c", kinds[k].name, 'a' + half);
        if (!all && strcmp(selected, name)) continue;
        if (!kinds[k].ni) {
            if (!all) fail("selected camera has no ISP-attached buffers");
            fprintf(stderr, "%s: absent\n", name);
            continue;
        }
        struct camera *c = &cameras[nc++];
        c->kind = &kinds[k];
        c->base = half * RING;
        strcpy(c->name, name);
        for (unsigned i = c->base; i < c->base + RING; ++i) {
            map_buffer(&kinds[k].meta[i]);
            if (!list) map_buffer(&kinds[k].image[i]);
        }
    }
    if (!nc) fail("no supported camera buffers found");
    alive();
    drop_root();
    fprintf(stderr, "XRService pid=%u; %u camera(s); output uid=%u. Association is inferred; no tear-free guarantee.\n",
            pid, nc, (unsigned)geteuid());
    if (list) {
        puts("camera\tslot\timage_fd\timage_inode\tmetadata_fd\tmetadata_inode\tmetadata_first_u64");
        for (unsigned c = 0; c < nc; ++c) for (unsigned i = cameras[c].base; i < cameras[c].base + RING; ++i) {
            struct kind *k = cameras[c].kind;
            printf("%s\t%u\t%d\t%" PRIu64 "\t%d\t%" PRIu64 "\t%" PRIu64 "\n",
                   cameras[c].name, i - cameras[c].base, k->image[i].source_fd, k->image[i].ino,
                   k->meta[i].source_fd, k->meta[i].ino, marker(&k->meta[i]));
        }
        alive();
    } else {
        char default_dir[128], stamp[32];
        if (!output) {
            time_t t = time(NULL);
            struct tm tm;
            if (!localtime_r(&t, &tm) || !strftime(stamp, sizeof(stamp), "%Y%m%d-%H%M%S", &tm))
                fail("cannot format output time");
            if (mkdir("captures", 0700) && errno != EEXIST) die("mkdir captures");
            snprintf(default_dir, sizeof(default_dir), "captures/tap-%s-%d", stamp, (int)getpid());
            output = default_dir;
        }
        if (mkdir(output, 0700)) die("create NEW output directory (parent must exist)");
        int directory = open(output, O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW);
        if (directory < 0) die("open output directory");
        printf("Output: %s\n", output);
        for (unsigned c = 0; c < nc; ++c) {
            uint64_t second;
            select_slot(&cameras[c], &second, &cameras[c].last);
            cameras[c].deadline = now_ns() + 5000000000ULL;
        }
        unsigned remaining = nc;
        while (remaining) {
            alive();
            for (unsigned c = 0; c < nc; ++c) {
                struct camera *camera = &cameras[c];
                if (camera->saved == count) continue;
                uint64_t value, newest;
                unsigned slot = select_slot(camera, &value, &newest);
                if (value > camera->last && capture(camera, slot, value, directory, pid, full)) {
                    if (camera->saved == count) --remaining;
                } else if (now_ns() > camera->deadline) {
                    fprintf(stderr, "%s: no advancing, stable metadata marker within 5 seconds\n", camera->name);
                    fail("stream paused, layout changed, or capture raced DMA; no stale frame substituted");
                }
            }
            if (remaining) {
                struct timespec delay = {.tv_nsec = 20000000};
                while (nanosleep(&delay, &delay) && errno == EINTR) {}
            }
        }
        if (close(directory)) die("close output directory");
    }
    for (unsigned c = 0; c < nc; ++c) for (unsigned i = cameras[c].base; i < cameras[c].base + RING; ++i) {
        struct buffer *b[] = {&cameras[c].kind->image[i], &cameras[c].kind->meta[i]};
        for (unsigned n = 0; n < 2; ++n) if (b[n]->fd >= 0) {
            munmap(b[n]->map, b[n]->size);
            close(b[n]->fd);
        }
    }
    close(target_pidfd);
    return 0;
}
