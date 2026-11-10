/*
 * Copyright (C) 2026 The LineageOS Project
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

/*
 * Let the hammerhead ISP module run a stream on both VFEs.
 *
 * Above the pixel clock of one VFE the module splits the frame into two
 * overlapping stripes, one for each VFE. The hammerhead never needed that,
 * and two things break:
 *
 * - The module gives each VFE the columns of the focus (BF) statistics grid
 *   that fall into its stripe. It needs a column boundary inside the overlap
 *   for that, and refuses to start the stream otherwise. The region the
 *   focus module asks for, the middle half of the frame in five columns, has
 *   none there. Move the region sideways by the least that puts one there,
 *   before the module configures the statistics.
 *
 * - Each VFE has a thread that handles its start of frame, and both work on
 *   the lists of frame numbers, parameters and metadata of the channel at
 *   the same time, which corrupts them. Run the functions that use the lists
 *   one at a time.
 *
 * - Both threads pass the start of frame on to the session. It expects two
 *   of them for each frame of a split stream, and sends the metadata and the
 *   start of frame to the other modules on every second one. But it takes
 *   the next parameters off its queue on each one, so two for a frame, and
 *   the frame the first asks for is never made: it applies the requests of
 *   the camera HAL to the frames to make, and loses one of two that come
 *   close together. The HAL keeps waiting for those frames, so it can never
 *   finish its requests, and does when the camera stops repeating them,
 *   after a picture. Take the parameters off the queue on the first start of
 *   each frame of a session only; the kernel gives both VFEs the same frame
 *   numbers.
 *
 * Load as a shim of the module. It calls these functions through its own
 * PLT, so point those slots at wrappers.
 */

#define LOG_TAG "isp_dual_shim"

#include <elf.h>
#include <link.h>
#include <pthread.h>
#include <stdint.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>

#include <log/log.h>

#define MODULE_NAME "libmmcamera2_isp_modules.so"
#define MODULE "/" MODULE_NAME

/* The action function bf_stats_open returns, in the M4B30X module */
#define BF_ACTION_OFFSET 0x2ea65

/* bf_stats_open returns a table of functions, with the action at */
#define OPS_ACTION 3

/* The action that configures the statistics */
#define ACTION_CONFIG 2

/* Fields of the parameters the action gets */
#define P_SPLIT 160 /* first pixel of the stripe of the right VFE */
#define P_DUAL 668
#define P_OVERLAP 672 /* the left stripe ends this far after P_SPLIT */
#define P_BF_H_NUM 1400
#define P_BF_LEFT 1408
#define P_BF_WIDTH 1416

/* The offset of the grid in a stripe must be above this */
#define MIN_OFFSET 7

/*
 * Any of the hooked functions. The wrappers pass on the four argument
 * registers and the first four words of arguments on the stack, and return
 * both result registers.
 */
typedef uint64_t (*func_t)(uint32_t, uint32_t, uint32_t, uint32_t, uint32_t, uint32_t,
        uint32_t, uint32_t);

static ElfW(Addr) module_base;
static int (*orig_bf_action)(void *, uint32_t, void *, void *);

static pthread_mutex_t list_lock = PTHREAD_RECURSIVE_MUTEX_INITIALIZER_NP;

static int32_t param(const uint8_t *p, int off)
{
    int32_t v;

    memcpy(&v, p + off, sizeof(v));
    return v;
}

/*
 * The checks the module makes, for a grid of n columns of width w starting
 * at x: split it at the last boundary within the left stripe, and see that
 * both parts fit their stripes.
 */
static int grid_fits(int32_t x, int32_t w, int32_t n, int32_t split, int32_t overlap)
{
    int32_t left_end = split + overlap;
    int32_t frame_end = 2 * split + overlap;
    int32_t end = x + n * w;
    int32_t last = end < left_end ? end : left_end;
    int32_t n_left;

    if (x <= MIN_OFFSET || end >= frame_end)
        return 0;

    n_left = last > x ? (last - x) / w : 0;
    if (n_left == n)
        return end < left_end;
    if (x + n_left * w - split <= MIN_OFFSET)
        return 0;
    return n_left == 0 || x + n_left * w < left_end;
}

static void fit_bf_region(uint8_t *p)
{
    int32_t split = param(p, P_SPLIT);
    int32_t overlap = param(p, P_OVERLAP);
    int32_t n = param(p, P_BF_H_NUM);
    int32_t left = param(p, P_BF_LEFT);
    int32_t x, w, d;

    if (!param(p, P_DUAL) || n <= 0 || split <= 0 || overlap <= 0)
        return;

    /* The register values the module makes of the region */
    x = (left + 8) & 0x1ffe;
    w = (param(p, P_BF_WIDTH) / n) & ~1;
    if (w <= 0 || grid_fits(x, w, n, split, overlap))
        return;

    for (d = 2; d <= w; d += 2) {
        if (grid_fits(x + d, w, n, split, overlap))
            break;
        if (grid_fits(x - d, w, n, split, overlap)) {
            d = -d;
            break;
        }
    }
    if (d > w) {
        ALOGE("no place for %d columns of %d at %d", n, w, x);
        return;
    }

    left += d;
    memcpy(p + P_BF_LEFT, &left, sizeof(left));
    ALOGI("moved %d columns of %d by %d to %d for the stripes at %d+%d", n, w, d, left, split,
            overlap);
}

static int bf_action(void *ctx, uint32_t action, void *params, void *arg)
{
    if (action == ACTION_CONFIG && params)
        fit_bf_region(params);
    return orig_bf_action(ctx, action, params, arg);
}

enum {
    BF_STATS_OPEN,
    NOTIFY_SOF,
    GET_FRAME_NUMBER,
    SET_SUPER_PARAM,
    STORE_NEW_PARM,
    REPORT_STORED_PARAM,
    STORE_NEW_META,
    REPORT_STORED_META,
    QUEUE_POP_HEAD,
    NUM_HOOKS,
};

static func_t orig[NUM_HOOKS];

static void *bf_stats_open(uint32_t a, uint32_t b, uint32_t c, uint32_t d, uint32_t e,
        uint32_t f, uint32_t g, uint32_t h)
{
    void **ops = (void **)(uintptr_t)orig[BF_STATS_OPEN](a, b, c, d, e, f, g, h);
    void *action;

    if (!ops)
        return ops;

    action = ops[OPS_ACTION];
    if ((ElfW(Addr))action != module_base + BF_ACTION_OFFSET) {
        ALOGE("unknown BF action %p, not hooking it", action);
        return ops;
    }
    orig_bf_action = action;
    ops[OPS_ACTION] = (void *)bf_action;
    return ops;
}

#define LOCKED(name, hook)                                                                   \
    static uint64_t name(uint32_t a, uint32_t b, uint32_t c, uint32_t d, uint32_t e,       \
            uint32_t f, uint32_t g, uint32_t h)                                            \
    {                                                                                      \
        uint64_t ret;                                                                      \
                                                                                           \
        pthread_mutex_lock(&list_lock);                                                    \
        ret = orig[hook](a, b, c, d, e, f, g, h);                                          \
        pthread_mutex_unlock(&list_lock);                                                  \
        return ret;                                                                        \
    }

/* The event notify_sof gets: the session and a pointer to the frame number */
#define SOF_SESSION 0
#define SOF_FRAME 3
#define MAX_SESSIONS 4
#define SOF_RESTART 30

static struct {
    uint32_t session;
    uint32_t frame;
} last_sof[MAX_SESSIONS];

/* Whether this start of frame was passed on already, and note it if not */
static int seen_sof(uint32_t session, uint32_t frame)
{
    int i, free_slot = -1;

    for (i = 0; i < MAX_SESSIONS; i++) {
        if (last_sof[i].session == session) {
            /*
             * The thread of one VFE can fall behind the other by a frame, so
             * a start of frame older than the last is one passed on already.
             * One far older is from a stream started again.
             */
            if (frame <= last_sof[i].frame && last_sof[i].frame - frame < SOF_RESTART)
                return 1;
            last_sof[i].frame = frame;
            return 0;
        }
        if (!last_sof[i].session && free_slot < 0)
            free_slot = i;
    }
    if (free_slot < 0)
        free_slot = session % MAX_SESSIONS;
    last_sof[free_slot].session = session;
    last_sof[free_slot].frame = frame;
    return 0;
}

/*
 * Set while notify_sof handles a start of frame passed on already. Its first
 * call of mct_queue_pop_head, before any other, takes the parameters.
 */
static __thread int skip_pop;

static uint64_t notify_sof(uint32_t a, uint32_t b, uint32_t c, uint32_t d, uint32_t e,
        uint32_t f, uint32_t g, uint32_t h)
{
    const uint32_t *ev = (const uint32_t *)(uintptr_t)b;
    uint64_t ret;

    pthread_mutex_lock(&list_lock);
    skip_pop = ev && ev[SOF_FRAME] &&
            seen_sof(ev[SOF_SESSION], *(const uint32_t *)(uintptr_t)ev[SOF_FRAME]);
    ret = orig[NOTIFY_SOF](a, b, c, d, e, f, g, h);
    skip_pop = 0;
    pthread_mutex_unlock(&list_lock);
    return ret;
}

static uint64_t queue_pop_head(uint32_t a, uint32_t b, uint32_t c, uint32_t d, uint32_t e,
        uint32_t f, uint32_t g, uint32_t h)
{
    if (skip_pop) {
        skip_pop = 0;
        return 0;
    }
    return orig[QUEUE_POP_HEAD](a, b, c, d, e, f, g, h);
}

LOCKED(get_frame_number, GET_FRAME_NUMBER)
LOCKED(set_super_param, SET_SUPER_PARAM)
LOCKED(store_new_parm, STORE_NEW_PARM)
LOCKED(report_stored_param, REPORT_STORED_PARAM)
LOCKED(store_new_meta, STORE_NEW_META)
LOCKED(report_stored_meta, REPORT_STORED_META)

static const struct {
    const char *name;
    ElfW(Addr) offset; /* where the slot points in the M4B30X module, if in it */
    void *wrapper;
} hooks[NUM_HOOKS] = {
    [BF_STATS_OPEN] = { "bf_stats_open", 0x2ee65, bf_stats_open },
    [NOTIFY_SOF] = { "isp_ch_util_hw_notify_sof", 0x37735, notify_sof },
    [GET_FRAME_NUMBER] = { "isp_ch_get_frame_number", 0x3707d, get_frame_number },
    [SET_SUPER_PARAM] = { "isp_set_super_param", 0x33655, set_super_param },
    [STORE_NEW_PARM] = { "isp_ch_util_store_new_parm", 0x37125, store_new_parm },
    [REPORT_STORED_PARAM] = { "isp_ch_util_report_stored_param", 0x371e5, report_stored_param },
    [STORE_NEW_META] = { "isp_ch_util_store_new_meta", 0x3726d, store_new_meta },
    [REPORT_STORED_META] = { "isp_ch_util_report_stored_meta", 0x37345, report_stored_meta },
    /* in another library, not checked */
    [QUEUE_POP_HEAD] = { "mct_queue_pop_head", 0, queue_pop_head },
};

struct module_info {
    ElfW(Addr) base;
    const ElfW(Dyn) *dynamic;
};

static int find_module(struct dl_phdr_info *info, size_t size, void *arg)
{
    struct module_info *m = arg;
    size_t len = info->dlpi_name ? strlen(info->dlpi_name) : 0;
    int i;

    (void)size;
    if (len < strlen(MODULE) || strcmp(info->dlpi_name + len - strlen(MODULE), MODULE))
        return 0;

    m->base = info->dlpi_addr;
    for (i = 0; i < info->dlpi_phnum; i++)
        if (info->dlpi_phdr[i].p_type == PT_DYNAMIC)
            m->dynamic = (const ElfW(Dyn) *)(info->dlpi_addr + info->dlpi_phdr[i].p_vaddr);
    return 1;
}

/* The PLT slots of the hooked functions */
static int find_plt_slots(const struct module_info *m, ElfW(Addr) *slots[NUM_HOOKS])
{
    const ElfW(Rel) *rel = NULL;
    const ElfW(Sym) *symtab = NULL;
    const char *strtab = NULL;
    size_t relsz = 0, i;
    const ElfW(Dyn) *d;
    int h;

    for (d = m->dynamic; d->d_tag != DT_NULL; d++) {
        switch (d->d_tag) {
        case DT_JMPREL:
            rel = (const ElfW(Rel) *)(m->base + d->d_un.d_ptr);
            break;
        case DT_PLTRELSZ:
            relsz = d->d_un.d_val;
            break;
        case DT_SYMTAB:
            symtab = (const ElfW(Sym) *)(m->base + d->d_un.d_ptr);
            break;
        case DT_STRTAB:
            strtab = (const char *)(m->base + d->d_un.d_ptr);
            break;
        }
    }
    if (!rel || !symtab || !strtab)
        return -1;

    for (i = 0; i < relsz / sizeof(*rel); i++) {
        const char *name;

        if (ELF32_R_TYPE(rel[i].r_info) != R_ARM_JUMP_SLOT)
            continue;
        name = strtab + symtab[ELF32_R_SYM(rel[i].r_info)].st_name;
        for (h = 0; h < NUM_HOOKS; h++)
            if (!strcmp(name, hooks[h].name))
                slots[h] = (ElfW(Addr) *)(m->base + rel[i].r_offset);
    }
    for (h = 0; h < NUM_HOOKS; h++) {
        if (!slots[h]) {
            ALOGE("%s does not call %s", MODULE_NAME, hooks[h].name);
            return -1;
        }
    }
    return 0;
}

__attribute__((constructor)) static void isp_dual_shim_init(void)
{
    struct module_info m = { 0, NULL };
    ElfW(Addr) *slots[NUM_HOOKS] = { NULL };
    long page_size = sysconf(_SC_PAGESIZE);
    ElfW(Addr) first = ~(ElfW(Addr))0, last = 0;
    int h;

    if (!dl_iterate_phdr(find_module, &m) || !m.dynamic) {
        ALOGE("%s not loaded", MODULE_NAME);
        return;
    }
    if (find_plt_slots(&m, slots))
        return;

    /* Make sure this is the module the offsets were taken from */
    for (h = 0; h < NUM_HOOKS; h++) {
        if (hooks[h].offset && *slots[h] != m.base + hooks[h].offset) {
            ALOGE("unknown %s, not hooking it", MODULE_NAME);
            return;
        }
        if ((ElfW(Addr))slots[h] < first)
            first = (ElfW(Addr))slots[h];
        if ((ElfW(Addr))slots[h] > last)
            last = (ElfW(Addr))slots[h];
    }

    /* The slots are in the part made read only after relocation */
    first &= ~(page_size - 1);
    last = (last + sizeof(ElfW(Addr)) + page_size - 1) & ~(page_size - 1);
    if (mprotect((void *)first, last - first, PROT_READ | PROT_WRITE)) {
        ALOGE("could not make the PLT slots writable");
        return;
    }
    module_base = m.base;
    for (h = 0; h < NUM_HOOKS; h++) {
        orig[h] = (func_t)*slots[h];
        *slots[h] = (ElfW(Addr))hooks[h].wrapper;
    }
    mprotect((void *)first, last - first, PROT_READ);
    ALOGI("hooked %d functions", NUM_HOOKS);
}
