// Определяет, реализует ли ядро AMDGPU_GEM_OP_SET_PRIORITY (op=3).
//
// Ловушка: в mainline amdgpu_gem_op_ioctl нет ветки default, а переменная r к
// моменту switch уже равна 0 после успешного drm_exec_lock_obj. Поэтому
// НЕизвестный op возвращает 0, а не -EINVAL -- и просто ничего не делает.
// Значит «вернулось 0» само по себе ничего не доказывает.
//
// Различаем так: зовём op=3 и заведомо мусорный op=200. Если оба дают 0 --
// ядро игнорирует незнакомое, op=3 не реализован. Если op=200 даёт ошибку, а
// op=3 нет -- реализован.
#include <stdio.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>
#include <errno.h>
#include <stdint.h>
#include <sys/ioctl.h>
#include <linux/types.h>

#define DRM_COMMAND_BASE 0x40
#define DRM_IOCTL_BASE 'd'
#define DRM_IOWR(nr,type) _IOWR(DRM_IOCTL_BASE,nr,type)

struct gem_create_in  { __u64 bo_size, alignment, domains, domain_flags; };
struct gem_create_out { __u32 handle; __u32 _pad; };
union gem_create { struct gem_create_in in; struct gem_create_out out; };
struct gem_op { __u32 handle, op; __u64 value; };

#define IOCTL_GEM_CREATE DRM_IOWR(DRM_COMMAND_BASE + 0x00, union gem_create)
#define IOCTL_GEM_OP     DRM_IOWR(DRM_COMMAND_BASE + 0x10, struct gem_op)
#define DOMAIN_VRAM 0x4

int main(void) {
    int fd = open("/dev/dri/renderD128", O_RDWR);
    if (fd < 0) { perror("open renderD128"); return 1; }

    union gem_create c; memset(&c, 0, sizeof c);
    c.in.bo_size = 4096; c.in.alignment = 4096; c.in.domains = DOMAIN_VRAM;
    if (ioctl(fd, IOCTL_GEM_CREATE, &c) < 0) { perror("GEM_CREATE"); close(fd); return 1; }
    __u32 h = c.out.handle;
    printf("буфер создан, handle=%u\n\n", h);

    struct { const char *name; __u32 op; } probes[] = {
        { "op=1  SET_PLACEMENT   (существует в mainline)", 1 },
        { "op=3  SET_PRIORITY    (патч OGC)             ", 3 },
        { "op=200 заведомо мусор                        ", 200 },
    };
    for (unsigned i = 0; i < sizeof probes / sizeof *probes; i++) {
        struct gem_op a; memset(&a, 0, sizeof a);
        a.handle = h; a.op = probes[i].op; a.value = 0;
        errno = 0;
        int r = ioctl(fd, IOCTL_GEM_OP, &a);
        printf("  %s -> ret=%d errno=%d (%s)\n",
               probes[i].name, r, r < 0 ? errno : 0, r < 0 ? strerror(errno) : "ok");
    }
    close(fd);
    return 0;
}
