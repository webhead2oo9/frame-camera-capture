// eyetap: save the owner's eye-camera frames from the running eyetracking service.
//
// The eye cameras are driven through the ADSP; frames land in a 16 MiB udmabuf that the
// eyetracking process shares with dsp_service, vrserver, XRService, and vrcompositor.
// This tool duplicates that buffer handle (pidfd_getfd, needs sudo), maps it read-only,
// and copies frames out. It takes no locks, writes nothing into the buffer, sends no
// ioctl to any camera or DSP device, and issues no DMA-buffer sync (the buffer is also
// written by CPU processes). Layout details and limits: ../README.md.
#define _GNU_SOURCE
#include <dirent.h>
#include <endian.h>
#include <errno.h>
#include <fcntl.h>
#include <getopt.h>
#include <grp.h>
#include <inttypes.h>
#include <limits.h>
#include <poll.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <time.h>
#include <unistd.h>

// Empirical layout, SteamVR runtime 2.18.1 (2026-09-29). Not a documented format.
enum {
    BUFFER_SIZE = 16777216,  // the shared udmabuf
    RING_BASE = 0x234000,    // slot 0; also holds the ring header
    SLOT_STEP = 0x40000,     // slot pitch
    CAMERAS = 2,             // a = right eye, b = left eye (verified on this unit)
    SLOTS_PER_CAMERA = 4,    // slots 0-3 camera a, 4-7 camera b
    WIDTH = 400,
    HEIGHT = 400,
    STRIDE = 512,            // 400 pixels + 112 padding bytes per row
    TIMESTAMP_BEFORE = 64,   // u64 LE frame timestamp, ns, unidentified clock
    HEADER_CAMERA_STEP = 16, // ring header: per-camera exposure, gain, rate floats
    HEADER_EXPOSURE = 8,
    HEADER_GAIN = 12,
    HEADER_RATE = 16,
};
static const uint64_t STALL_NS = 3000000000ULL;
static const uint64_t RING_SPAN_LIMIT_NS = 1000000000ULL;  // 4 frames at >= 15 fps

struct ring {
    unsigned pid;
    int pidfd, fd;
    uint64_t inode;
    const uint8_t *map;
};
struct camera {
    char name;  // 'a' or 'b'
    bool wanted;
    unsigned saved;
    uint64_t last, deadline;
};

// ---- errors and small helpers -------------------------------------------------------

static void fail(const char *message) {
    fprintf(stderr, "eyetap: %s\n", message);
    exit(1);
}
static void die(const char *operation) {
    fprintf(stderr, "eyetap: %s: %s\n", operation, strerror(errno));
    exit(1);
}
static bool numeric(const char *s) {
    return *s && strspn(s, "0123456789") == strlen(s);
}
static unsigned number(const char *s, unsigned min) {
    char *end;
    errno = 0;
    unsigned long v = strtoul(s, &end, 10);
    if (!numeric(s) || *end || errno || v < min || v > INT_MAX) fail("invalid numeric argument");
    return (unsigned)v;
}
static uint64_t now_ns(void) {
    struct timespec ts;
    if (clock_gettime(CLOCK_MONOTONIC, &ts)) die("clock_gettime");
    return (uint64_t)ts.tv_sec * 1000000000 + (uint64_t)ts.tv_nsec;
}
static void pause_ms(long ms) {
    struct timespec delay = {.tv_nsec = ms * 1000000};
    while (nanosleep(&delay, &delay) && errno == EINTR) {}
}

// ---- locating the eyetracking process and its buffer --------------------------------

static bool is_eyetracking(unsigned pid) {
    char path[64], exe[PATH_MAX];
    snprintf(path, sizeof(path), "/proc/%u/exe", pid);
    ssize_t n = readlink(path, exe, sizeof(exe) - 1);
    if (n < 0 || (size_t)n >= sizeof(exe) - 1) return false;
    exe[n] = 0;
    const char *base = strrchr(exe, '/');
    return base && !strcmp(base + 1, "eyetracking");
}
static unsigned find_eyetracking(void) {
    DIR *d = opendir("/proc");
    if (!d) die("opendir /proc");
    unsigned pid = 0;
    for (struct dirent *e; (e = readdir(d));) {
        if (!numeric(e->d_name)) continue;
        unsigned candidate = number(e->d_name, 1);
        if (!is_eyetracking(candidate)) continue;
        if (pid) fail("multiple eyetracking processes; select one with --pid");
        pid = candidate;
    }
    closedir(d);
    if (!pid) fail("no eyetracking process found; this tool does not start it");
    return pid;
}
static void check_alive(const struct ring *r) {
    struct pollfd p = {.fd = r->pidfd, .events = POLLIN};
    int rc;
    do { rc = poll(&p, 1, 0); } while (rc < 0 && errno == EINTR);
    if (rc < 0) die("poll(pidfd)");
    if (rc) fail("eyetracking exited; rerun after it restarts");
}
// Returns true for a udmabuf and fills its inode and size.
static bool udmabuf_info(const char *fdinfo_path, uint64_t *inode, size_t *size) {
    FILE *f = fopen(fdinfo_path, "r");
    if (!f) {
        if (errno == ENOENT) return false;
        die("read fdinfo");
    }
    char line[256], exporter[64] = "";
    *inode = 0;
    *size = 0;
    while (fgets(line, sizeof(line), f)) {
        if (sscanf(line, "ino: %" SCNu64, inode) == 1) continue;
        if (sscanf(line, "size: %zu", size) == 1) continue;
        (void)sscanf(line, "exp_name: %63s", exporter);
    }
    if (ferror(f)) die("read fdinfo");
    fclose(f);
    return *size && !strcmp(exporter, "udmabuf");
}
// The process holds the buffer through several fds; require one distinct object.
static int find_ring_fd(unsigned pid, uint64_t *inode_out) {
    char directory[64], path[128];
    snprintf(directory, sizeof(directory), "/proc/%u/fdinfo", pid);
    DIR *d = opendir(directory);
    if (!d) die("opendir eyetracking fdinfo");
    int chosen = -1;
    for (struct dirent *e; (e = readdir(d));) {
        if (!numeric(e->d_name)) continue;
        unsigned fd = number(e->d_name, 0);
        snprintf(path, sizeof(path), "%s/%u", directory, fd);
        uint64_t inode;
        size_t size;
        if (!udmabuf_info(path, &inode, &size) || size != BUFFER_SIZE) continue;
        if (chosen < 0) {
            chosen = (int)fd;
            *inode_out = inode;
        } else if (inode != *inode_out) {
            fail("several 16 MiB udmabufs; layout changed, refusing to guess");
        }
    }
    closedir(d);
    if (chosen < 0) fail("eyetracking holds no 16 MiB udmabuf; layout changed or tracker not initialised");
    return chosen;
}
static struct ring open_ring(unsigned pid) {
    struct ring r = {.pid = pid ? pid : find_eyetracking()};
    r.pidfd = (int)syscall(SYS_pidfd_open, r.pid, 0);
    if (r.pidfd < 0) die("pidfd_open");
    if (!is_eyetracking(r.pid)) fail("target executable is not eyetracking");
    int source_fd = find_ring_fd(r.pid, &r.inode);
    r.fd = (int)syscall(SYS_pidfd_getfd, r.pidfd, source_fd, 0);
    if (r.fd < 0) die("pidfd_getfd (requires ptrace permission; normally sudo)");
    char path[64];
    uint64_t inode;
    size_t size;
    snprintf(path, sizeof(path), "/proc/self/fdinfo/%d", r.fd);
    if (!udmabuf_info(path, &inode, &size) || inode != r.inode || size != BUFFER_SIZE)
        fail("source fd changed during discovery; rerun");
    void *map = mmap(NULL, BUFFER_SIZE, PROT_READ, MAP_SHARED, r.fd, 0);
    if (map == MAP_FAILED) die("mmap(PROT_READ)");
    r.map = map;
    check_alive(&r);
    return r;
}
static void close_ring(struct ring *r) {
    munmap((void *)r->map, BUFFER_SIZE);
    close(r->fd);
    close(r->pidfd);
}
// Only the handle duplication needs root; write output as the sudo caller.
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

// ---- ring layout --------------------------------------------------------------------

static unsigned first_slot(const struct camera *c) {
    return (unsigned)(c->name - 'a') * SLOTS_PER_CAMERA;
}
// Each slot's image start shifts by 64 bytes per slot, plus 64 more for camera b.
static size_t image_offset(unsigned slot) {
    return RING_BASE + (size_t)slot * SLOT_STEP + 256 + 64 * slot + (slot >= SLOTS_PER_CAMERA ? 64 : 0);
}
static uint64_t frame_timestamp(const struct ring *r, unsigned slot) {
    uint64_t v;
    __atomic_thread_fence(__ATOMIC_ACQUIRE);
    memcpy(&v, r->map + image_offset(slot) - TIMESTAMP_BEFORE, sizeof(v));
    __atomic_thread_fence(__ATOMIC_ACQUIRE);
    return le64toh(v);
}
static float header_float(const struct ring *r, const struct camera *c, unsigned field) {
    float v;
    memcpy(&v, r->map + RING_BASE + field + (unsigned)(c->name - 'a') * HEADER_CAMERA_STEP, sizeof(v));
    return v;
}
// Second-newest distinct timestamp for a camera; the newest slot may still be written.
static unsigned settled_slot(const struct ring *r, const struct camera *c, uint64_t *timestamp) {
    unsigned slot = first_slot(c), newest_slot = slot;
    uint64_t newest = 0;
    *timestamp = 0;
    for (unsigned i = first_slot(c); i < first_slot(c) + SLOTS_PER_CAMERA; ++i) {
        uint64_t v = frame_timestamp(r, i);
        if (v > newest) {
            *timestamp = newest;
            slot = newest_slot;
            newest = v;
            newest_slot = i;
        } else if (v < newest && v > *timestamp) {
            *timestamp = v;
            slot = i;
        }
    }
    return slot;
}
static void validate_layout(const struct ring *r) {
    for (unsigned s = 0; s < CAMERAS * SLOTS_PER_CAMERA; s += SLOTS_PER_CAMERA) {
        uint64_t lo = UINT64_MAX, hi = 0;
        for (unsigned i = s; i < s + SLOTS_PER_CAMERA; ++i) {
            uint64_t v = frame_timestamp(r, i);
            if (!v) fail("zero frame timestamp; layout changed or eye tracking never ran");
            if (v < lo) lo = v;
            if (v > hi) hi = v;
        }
        if (hi - lo > RING_SPAN_LIMIT_NS) fail("eye ring timestamps inconsistent; layout changed, refusing to guess");
    }
}

// ---- output -------------------------------------------------------------------------

static void write_all(int fd, const void *data, size_t size) {
    for (const unsigned char *p = data; size;) {
        ssize_t n = write(fd, p, size);
        if (n < 0 && errno == EINTR) continue;
        if (n <= 0) die("write frame");
        p += n;
        size -= (size_t)n;
    }
}
static void write_new_file(int directory, const char *name, const void *data, size_t size) {
    int fd = openat(directory, name, O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC | O_NOFOLLOW, 0600);
    if (fd < 0) die("create capture file");
    write_all(fd, data, size);
    if (close(fd)) die("close capture file");
}
// Rename NAME.part to NAME; a .part file is never a complete frame.
static void publish(int directory, const char *part) {
    char name[128];
    size_t n = strlen(part);
    if (n < 5 || n >= sizeof(name)) fail("invalid internal filename");
    memcpy(name, part, n - 5);
    name[n - 5] = 0;
    if (renameat(directory, part, directory, name)) die("publish capture file");
}
static int create_output_dir(const char *output) {
    char default_dir[128], stamp[32];
    if (!output) {
        time_t t = time(NULL);
        struct tm tm;
        if (!localtime_r(&t, &tm) || !strftime(stamp, sizeof(stamp), "%Y%m%d-%H%M%S", &tm))
            fail("cannot format output time");
        if (mkdir("captures", 0700) && errno != EEXIST) die("mkdir captures");
        snprintf(default_dir, sizeof(default_dir), "captures/eyes-%s-%d", stamp, (int)getpid());
        output = default_dir;
    }
    if (mkdir(output, 0700)) die("create NEW output directory (parent must exist)");
    int directory = open(output, O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW);
    if (directory < 0) die("open output directory");
    printf("Output: %s\n", output);
    return directory;
}

// ---- capture ------------------------------------------------------------------------

// Saves the camera's next settled frame, if a new one is available. Returns false when
// there is nothing new or the slot was rewritten during the copy (copy discarded).
static bool save_next_frame(const struct ring *r, struct camera *c, int directory) {
    uint64_t timestamp;
    unsigned slot = settled_slot(r, c, &timestamp);
    if (timestamp <= c->last) return false;

    static uint8_t pgm[32 + WIDTH * HEIGHT];
    int header = snprintf((char *)pgm, 32, "P5\n%d %d\n255\n", WIDTH, HEIGHT);
    const uint8_t *src = r->map + image_offset(slot);
    uint64_t begin = now_ns();
    for (unsigned row = 0; row < HEIGHT; ++row)
        memcpy(pgm + header + row * WIDTH, src + row * STRIDE, WIDTH);
    uint64_t end = now_ns();
    if (frame_timestamp(r, slot) != timestamp) return false;

    char image[64], record[64], json[1024];
    snprintf(image, sizeof(image), "eye-%c-%06u.pgm.part", c->name, c->saved + 1);
    snprintf(record, sizeof(record), "eye-%c-%06u.json.part", c->name, c->saved + 1);
    int n = snprintf(json, sizeof(json),
        "{\n  \"camera\": \"eye-%c\", \"eyetracking_pid\": %u, \"buffer_inode\": %" PRIu64 ",\n"
        "  \"slot\": %u, \"format\": \"gray8-pgm\", \"width\": %d, \"height\": %d,\n"
        "  \"frame_timestamp_ns\": \"%" PRIu64 "\", \"timestamp_clock\": \"unidentified\",\n"
        "  \"copy_begin_monotonic_ns\": %" PRIu64 ", \"copy_end_monotonic_ns\": %" PRIu64 ",\n"
        "  \"ring_header_inferred\": {\"exposure_s\": %.9g, \"gain\": %.9g, \"frame_rate\": %.9g},\n"
        "  \"tear_free_guaranteed\": false\n}\n",
        c->name, r->pid, r->inode, slot, WIDTH, HEIGHT, timestamp, begin, end,
        (double)header_float(r, c, HEADER_EXPOSURE), (double)header_float(r, c, HEADER_GAIN),
        (double)header_float(r, c, HEADER_RATE));
    if (n < 0 || (size_t)n >= sizeof(json)) fail("capture metadata overflow");

    write_new_file(directory, image, pgm, (size_t)header + WIDTH * HEIGHT);
    write_new_file(directory, record, json, (size_t)n);
    publish(directory, image);
    publish(directory, record);  // JSON last: it marks the frame complete
    ++c->saved;
    c->last = timestamp;
    c->deadline = end + STALL_NS;
    printf("eye-%c frame=%u slot=%u timestamp=%" PRIu64 "\n", c->name, c->saved, slot, timestamp);
    return true;
}
static void capture(const struct ring *r, struct camera *cameras, unsigned count, const char *output) {
    int directory = create_output_dir(output);
    unsigned remaining = 0;
    for (struct camera *c = cameras; c < cameras + CAMERAS; ++c) {
        if (!c->wanted) continue;
        settled_slot(r, c, &c->last);  // only frames arriving after startup count
        c->deadline = now_ns() + STALL_NS;
        ++remaining;
    }
    while (remaining) {
        check_alive(r);
        for (struct camera *c = cameras; c < cameras + CAMERAS; ++c) {
            if (!c->wanted || c->saved == count) continue;
            if (save_next_frame(r, c, directory)) {
                if (c->saved == count) --remaining;
            } else if (now_ns() > c->deadline) {
                fprintf(stderr, "eye-%c: no new frame within 3 seconds\n", c->name);
                fail("eye tracking paused (headset off-head?) or layout changed; no stale frame substituted");
            }
        }
        if (remaining) pause_ms(2);
    }
    if (close(directory)) die("close output directory");
}
static void list_slots(const struct ring *r) {
    puts("camera\tslot\tframe_timestamp_ns");
    for (unsigned i = 0; i < CAMERAS * SLOTS_PER_CAMERA; ++i)
        printf("eye-%c\t%u\t%" PRIu64 "\n", 'a' + i / SLOTS_PER_CAMERA, i, frame_timestamp(r, i));
}

// ---- command line -------------------------------------------------------------------

static void usage(void) {
    puts("Usage: eyetap [--pid PID] [--eye a|b|all] [--count N] [--output NEW_DIR] [--list]\n"
         "Default: one new 400x400 frame per eye camera, under captures/eyes-TIME-PID.\n"
         "Camera a = right eye, b = left eye (verified on the development unit).\n"
         "Requires sudo for pidfd_getfd; drops to the sudo user before output.\n"
         "Headset must be worn: eye tracking stops when the HMD is off-head.\n"
         "Experimental: empirical layout, unidentified timestamp clock, no producer fence.\n"
         "Never opens eye cameras, locks or writes shared memory, or controls services.");
}
int main(int argc, char **argv) {
    setvbuf(stdout, NULL, _IOLBF, 0);  // keep progress lines in order with stderr
    unsigned pid = 0, count = 1;
    const char *eye = "all", *output = NULL;
    bool list = false;
    static const struct option options[] = {
        {"pid", required_argument, NULL, 'p'}, {"eye", required_argument, NULL, 'e'},
        {"count", required_argument, NULL, 'n'}, {"output", required_argument, NULL, 'o'},
        {"list", no_argument, NULL, 'l'}, {"help", no_argument, NULL, 'h'}, {NULL, 0, NULL, 0}
    };
    for (int opt; (opt = getopt_long(argc, argv, "p:e:n:o:lh", options, NULL)) != -1;) {
        switch (opt) {
            case 'p': pid = number(optarg, 1); break;
            case 'e': eye = optarg; break;
            case 'n': count = number(optarg, 1); break;
            case 'o': output = optarg; break;
            case 'l': list = true; break;
            case 'h': usage(); return 0;
            default: usage(); return 2;
        }
    }
    if (optind != argc) { usage(); return 2; }
    bool all = !strcmp(eye, "all");
    struct camera cameras[CAMERAS] = {
        {.name = 'a', .wanted = all || !strcmp(eye, "a")},
        {.name = 'b', .wanted = all || !strcmp(eye, "b")},
    };
    if (!cameras[0].wanted && !cameras[1].wanted) fail("unknown eye; use a, b, or all");

    struct ring ring = open_ring(pid);
    drop_root();
    validate_layout(&ring);
    fprintf(stderr, "eyetracking pid=%u buffer inode=%" PRIu64 "; output uid=%u. Layout is empirical.\n",
            ring.pid, ring.inode, (unsigned)geteuid());
    if (list) list_slots(&ring);
    else capture(&ring, cameras, count, output);
    close_ring(&ring);
    return 0;
}
