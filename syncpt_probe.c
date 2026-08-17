/* Standalone probe: does dc->vblank_syncpt (TEGRA_DC_EXT_GET_VBLANK_SYNCPT)
 * actually increment at the real ~60Hz vblank rate, independent of the
 * FLIP4 layer? Opens tegradc.1, resolves the syncpt id, then samples its
 * current value (NVHOST_IOCTL_CTRL_SYNCPT_READ) once per ~100ms for 2s. */
#include <stdio.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/ioctl.h>
#include <time.h>
#define __user
#include "tegra_dc_ext.h"
#include "uapi/linux/nvhost_ioctl.h"

int main(void) {
    int dc_fd = open("/dev/tegra_dc_1", O_RDWR);
    if (dc_fd < 0) { perror("open tegra_dc_1"); return 1; }

    __u32 syncpt_id = 0xFFFFFFFF;
    if (ioctl(dc_fd, TEGRA_DC_EXT_GET_VBLANK_SYNCPT, &syncpt_id) < 0) {
        perror("GET_VBLANK_SYNCPT"); return 1;
    }
    printf("vblank syncpt id = %u\n", syncpt_id);

    int ctrl_fd = open("/dev/nvhost-ctrl", O_RDWR);
    if (ctrl_fd < 0) { perror("open nvhost-ctrl"); return 1; }

    struct timespec t0; clock_gettime(CLOCK_MONOTONIC, &t0);
    for (int i = 0; i < 20; i++) {
        struct nvhost_ctrl_syncpt_read_args rd = { .id = syncpt_id };
        if (ioctl(ctrl_fd, NVHOST_IOCTL_CTRL_SYNCPT_READ, &rd) < 0) {
            perror("SYNCPT_READ"); return 1;
        }
        struct timespec ts; clock_gettime(CLOCK_MONOTONIC, &ts);
        double t = (ts.tv_sec - t0.tv_sec) + (ts.tv_nsec - t0.tv_nsec) / 1e9;
        printf("t=%.3f value=%u\n", t, rd.value);
        usleep(100000);
    }

    printf("\n--- WAITEX test: blocking wait for each successive +1 target, 20x ---\n");
    struct nvhost_ctrl_syncpt_read_args rd0 = { .id = syncpt_id };
    if (ioctl(ctrl_fd, NVHOST_IOCTL_CTRL_SYNCPT_READ, &rd0) < 0) { perror("SYNCPT_READ"); return 1; }
    __u32 target = rd0.value + 1;
    clock_gettime(CLOCK_MONOTONIC, &t0);
    for (int i = 0; i < 20; i++) {
        struct nvhost_ctrl_syncpt_waitex_args wa = {0};
        wa.id = syncpt_id;
        wa.thresh = target;
        wa.timeout = 1000;
        struct timespec ts_before; clock_gettime(CLOCK_MONOTONIC, &ts_before);
        int r = ioctl(ctrl_fd, NVHOST_IOCTL_CTRL_SYNCPT_WAITEX, &wa);
        struct timespec ts_after; clock_gettime(CLOCK_MONOTONIC, &ts_after);
        double dt = (ts_after.tv_sec - ts_before.tv_sec) + (ts_after.tv_nsec - ts_before.tv_nsec) / 1e9;
        double t = (ts_after.tv_sec - t0.tv_sec) + (ts_after.tv_nsec - t0.tv_nsec) / 1e9;
        printf("t=%.3f target=%u ret=%d value=%u call_took=%.3fms\n",
               t, target, r, wa.value, dt * 1000.0);
        target += 1;
    }
    return 0;
}
