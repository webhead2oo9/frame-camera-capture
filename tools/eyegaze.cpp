// Public OpenVR gaze consumer. No camera service, compositor, or sensor controls.
#include <openvr.h>
#include <algorithm>
#include <cerrno>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fcntl.h>
#include <filesystem>
#include <getopt.h>
#include <limits.h>
#include <thread>
#include <unistd.h>

static const char *input_error(vr::EVRInputError error) {
    switch (error) {
        case vr::VRInputError_None: return "None";
        case vr::VRInputError_NameNotFound: return "NameNotFound";
        case vr::VRInputError_WrongType: return "WrongType";
        case vr::VRInputError_InvalidHandle: return "InvalidHandle";
        case vr::VRInputError_InvalidParam: return "InvalidParam";
        case vr::VRInputError_NoSteam: return "NoSteam";
        case vr::VRInputError_IPCError: return "IPCError";
        case vr::VRInputError_NoActiveActionSet: return "NoActiveActionSet";
        case vr::VRInputError_NoData: return "NoData";
        case vr::VRInputError_MismatchedActionManifest: return "MismatchedActionManifest";
        case vr::VRInputError_PermissionDenied: return "PermissionDenied";
        default: return "OtherInputError";
    }
}
static bool check(vr::EVRInputError error, const char *operation) {
    if (error == vr::VRInputError_None) return true;
    std::fprintf(stderr, "%s: %s (%d)\n", operation, input_error(error), error);
    return false;
}
static unsigned positive(const char *s) {
    char *end;
    errno = 0;
    unsigned long n = std::strtoul(s, &end, 10);
    if (!*s || *s < '0' || *s > '9' || *end || errno || !n || n > INT_MAX) {
        std::fprintf(stderr, "Invalid positive integer: %s\n", s);
        std::exit(1);
    }
    return static_cast<unsigned>(n);
}
static void usage() {
    std::puts("Usage: eyegaze [--seconds N] [--interval-ms N] [--output NEW.csv]\n"
              "Defaults: 10 seconds, 10 ms polling, CSV on stdout. Run without sudo.\n"
              "Uses the eyegaze_actions.json beside this executable and existing SteamVR.\n"
              "Gaze origin/target use the standing tracking universe (metres).\n"
              "head_* columns rotate the gaze ray into the HMD frame (+x right, +y up, -z forward);\n"
              "yaw is positive right, pitch positive up. Blank when gaze or HMD pose is invalid.\n"
              "Rows are API observations, not distinct camera frames or sensor timestamps.\n"
              "Exit: 0 = tracked data observed, 2 = no usable gaze, 1 = API/I/O/argument error.");
}
struct vr_session {
    ~vr_session() { vr::VR_Shutdown(); }
};
struct output_file {
    FILE *stream;
    ~output_file() { if (stream != stdout) std::fclose(stream); }
};
int main(int argc, char **argv) {
    unsigned seconds = 10, interval_ms = 10;
    const char *output = nullptr;
    static const option options[] = {
        {"seconds", required_argument, nullptr, 's'},
        {"interval-ms", required_argument, nullptr, 'i'},
        {"output", required_argument, nullptr, 'o'},
        {"help", no_argument, nullptr, 'h'}, {nullptr, 0, nullptr, 0}
    };
    int opt;
    while ((opt = getopt_long(argc, argv, "s:i:o:h", options, nullptr)) != -1) {
        switch (opt) {
            case 's': seconds = positive(optarg); break;
            case 'i': interval_ms = positive(optarg); break;
            case 'o': output = optarg; break;
            case 'h': usage(); return 0;
            default: usage(); return 1;
        }
    }
    if (optind != argc) { usage(); return 1; }
    if (geteuid() == 0) {
        std::fputs("Run as the desktop user, without sudo.\n", stderr);
        return 1;
    }
    char exe[PATH_MAX];
    ssize_t length = readlink("/proc/self/exe", exe, sizeof(exe) - 1);
    if (length < 0 || static_cast<size_t>(length) == sizeof(exe) - 1) {
        std::fputs("Cannot resolve executable path.\n", stderr);
        return 1;
    }
    exe[length] = 0;
    const auto manifest = std::filesystem::path(exe).parent_path() / "eyegaze_actions.json";
    if (access(manifest.c_str(), R_OK)) { std::perror("read action manifest"); return 1; }

    vr::EVRInitError init_error = vr::VRInitError_None;
    auto *system = vr::VR_Init(&init_error, vr::VRApplication_Background);
    if (init_error != vr::VRInitError_None || !system) {
        std::fprintf(stderr, "VR_Init: %s\n", vr::VR_GetVRInitErrorAsEnglishDescription(init_error));
        return 1;
    }
    vr_session session;
    auto *input = static_cast<vr::IVRInput *>(vr::VR_GetGenericInterface(vr::IVRInput_Version, &init_error));
    if (!input || init_error != vr::VRInitError_None) {
        std::fprintf(stderr, "%s: %s\n", vr::IVRInput_Version,
                     vr::VR_GetVRInitErrorAsEnglishDescription(init_error));
        return 1;
    }
    vr::ETrackedPropertyError property_error = vr::TrackedProp_Success;
    bool supported = system->GetBoolTrackedDeviceProperty(vr::k_unTrackedDeviceIndex_Hmd,
        vr::Prop_SupportsXrEyeGazeInteraction_Bool, &property_error);
    std::fprintf(stderr, "Eye gaze support=%d (%s); %s; origin=standing; poll=%u ms\n",
        supported, system->GetPropErrorNameFromEnum(property_error), vr::IVRInput_Version, interval_ms);
    if (property_error != vr::TrackedProp_Success || !supported) {
        std::fputs("HMD does not advertise eye gaze support.\n", stderr);
        return 1;
    }
    if (!check(input->SetActionManifestPath(manifest.c_str()), "SetActionManifestPath")) return 1;
    vr::VRActionSetHandle_t set = vr::k_ulInvalidActionSetHandle;
    vr::VRActionHandle_t action = vr::k_ulInvalidActionHandle;
    if (!check(input->GetActionSetHandle("/actions/gaze", &set), "GetActionSetHandle") ||
        !check(input->GetActionHandle("/actions/gaze/in/eye_gaze", &action), "GetActionHandle")) return 1;
    vr::VRActiveActionSet_t active{};
    active.ulActionSet = set;
    active.ulRestrictedToDevice = vr::k_ulInvalidInputValueHandle;
    // Default priority; no controller/button bindings that could consume other apps' input.

    FILE *stream = stdout;
    if (output) {
        int fd = open(output, O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC | O_NOFOLLOW, 0600);
        if (fd < 0) { std::perror("create NEW output CSV"); return 1; }
        stream = fdopen(fd, "w");
        if (!stream) { std::perror("fdopen"); close(fd); return 1; }
    }
    output_file file{stream};
    std::fputs("host_monotonic_ns,input_error,active,valid,tracked,usable,hmd_activity,head_pose_valid,origin_x_m,origin_y_m,origin_z_m,target_x_m,target_y_m,target_z_m,head_dir_x,head_dir_y,head_dir_z,head_yaw_deg,head_pitch_deg\n", stream);
    using clock = std::chrono::steady_clock;
    const auto finish = clock::now() + std::chrono::seconds(seconds);
    const auto interval = std::chrono::milliseconds(interval_ms);
    auto next = clock::now();
    unsigned long long rows = 0, usable_rows = 0, active_rows = 0;
    while (clock::now() < finish) {
        if (!check(input->UpdateActionState(&active, sizeof(active), 1), "UpdateActionState")) return 1;
        vr::VREyeTrackingData_t gaze{};
        const auto error = input->GetEyeTrackingDataRelativeToNow(action,
            vr::TrackingUniverseStanding, 0.0f, &gaze, sizeof(gaze));
        if (error != vr::VRInputError_None && error != vr::VRInputError_NoData) {
            check(error, "GetEyeTrackingDataRelativeToNow");
            return 1;
        }
        bool usable = error == vr::VRInputError_None && gaze.bActive && gaze.bValid && gaze.bTracked;
        for (unsigned i = 0; i < 3; ++i)
            usable = usable && std::isfinite(gaze.vGazeOrigin.v[i]) && std::isfinite(gaze.vGazeTarget.v[i]);
        const auto stamp = std::chrono::duration_cast<std::chrono::nanoseconds>(clock::now().time_since_epoch()).count();
        const int activity = system->GetTrackedDeviceActivityLevel(vr::k_unTrackedDeviceIndex_Hmd);
        vr::TrackedDevicePose_t head{};
        system->GetDeviceToAbsoluteTrackingPose(vr::TrackingUniverseStanding, 0.0f, &head, 1);
        const bool head_valid = head.bPoseIsValid && head.eTrackingResult == vr::TrackingResult_Running_OK;
        std::fprintf(stream, "%lld,%d,%d,%d,%d,%d,%d,%d", static_cast<long long>(stamp), error,
                     gaze.bActive, gaze.bValid, gaze.bTracked, usable, activity, head_valid);
        if (usable) {
            for (float v : gaze.vGazeOrigin.v) std::fprintf(stream, ",%.9g", v);
            for (float v : gaze.vGazeTarget.v) std::fprintf(stream, ",%.9g", v);
            ++usable_rows;
        } else {
            // Invalid samples must not masquerade as a gaze ray at the origin.
            std::fputs(",,,,,,", stream);
        }
        double world[3], local[3] = {0, 0, 0}, length = 0;
        for (unsigned i = 0; i < 3; ++i) world[i] = double(gaze.vGazeTarget.v[i]) - gaze.vGazeOrigin.v[i];
        // Rotation part of the device-to-standing matrix is orthonormal: inverse = transpose.
        for (unsigned c = 0; c < 3; ++c) {
            for (unsigned r = 0; r < 3; ++r) local[c] += double(head.mDeviceToAbsoluteTracking.m[r][c]) * world[r];
            length += local[c] * local[c];
        }
        length = std::sqrt(length);
        if (usable && head_valid && length > 1e-6) {
            for (double &v : local) v /= length;
            const double yaw = std::atan2(local[0], -local[2]) * 180.0 / M_PI;
            const double pitch = std::atan2(local[1], std::hypot(local[0], local[2])) * 180.0 / M_PI;
            std::fprintf(stream, ",%.6f,%.6f,%.6f,%.3f,%.3f", local[0], local[1], local[2], yaw, pitch);
        } else {
            std::fputs(",,,,,", stream);
        }
        std::fputc('\n', stream);
        if (std::ferror(stream)) { std::perror("write CSV"); return 1; }
        ++rows;
        if (gaze.bActive) ++active_rows;
        next += interval;
        // Do not burst-poll to catch up after scheduler or API delays.
        if (next < clock::now()) next = clock::now() + interval;
        std::this_thread::sleep_until(std::min(next, finish));
    }
    if (std::fflush(stream)) { std::perror("flush CSV"); return 1; }
    std::fprintf(stderr, "Observations=%llu active=%llu usable_tracked=%llu\n", rows, active_rows, usable_rows);
    if (!usable_rows) {
        std::fputs("No usable gaze observed. Wear/wake the HMD; check action activation and eyetracking service logs.\n", stderr);
        return 2;
    }
    return 0;
}
