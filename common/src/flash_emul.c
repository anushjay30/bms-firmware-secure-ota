/*
 * flash_emul.c - The flash HAL, emulated on RAM with real-flash rules.
 */
#include "flash_hal.h"

#include <stddef.h>

#define MAX_PROTECTED 4u

typedef struct {
    uint32_t addr;
    uint32_t len;
} range_t;

static struct {
    uintptr_t mem;  /* where flash address `base` is stored */
    uint32_t  base;
    uint32_t  size;
    range_t   protected_ranges[MAX_PROTECTED];
    unsigned  num_protected;

    flash_fault_t fault;
    uint32_t      fault_budget; /* bytes left before the armed fault fires */
    bool          dead;         /* power lost, not restored yet           */
    void        (*power_loss_handler)(void);

    flash_stats_t stats;
} s_flash;

void flash_emul_init(uintptr_t mem, uint32_t base, uint32_t size)
{
    s_flash.mem = mem;
    s_flash.base = base;
    s_flash.size = size;
    s_flash.num_protected = 0u;
    s_flash.fault = FLASH_FAULT_NONE;
    s_flash.fault_budget = 0u;
    s_flash.dead = false;
    s_flash.power_loss_handler = NULL;
    s_flash.stats.bytes_programmed = 0u;
    s_flash.stats.pages_erased = 0u;
}

/* Integer arithmetic, then one conversion: on the target `mem` is 0 (the
 * flash is identity-mapped), and arithmetic on a null pointer is undefined. */
static uint8_t *cell(uint32_t addr)
{
    return (uint8_t *)(s_flash.mem + (uintptr_t)(addr - s_flash.base));
}

static bool in_range(uint32_t addr, uint32_t len)
{
    return addr >= s_flash.base && len <= s_flash.size && addr - s_flash.base <= s_flash.size - len;
}

static bool is_protected(uint32_t addr, uint32_t len)
{
    for (unsigned i = 0; i < s_flash.num_protected; i++) {
        const range_t *r = &s_flash.protected_ranges[i];
        if (addr < r->addr + r->len && r->addr < addr + len) {
            return true; /* overlap */
        }
    }
    return false;
}

const uint8_t *flash_ptr(uint32_t addr)
{
    return in_range(addr, 1u) ? cell(addr) : NULL;
}

bool flash_protect(uint32_t addr, uint32_t len)
{
    if (s_flash.num_protected >= MAX_PROTECTED) {
        return false;
    }
    s_flash.protected_ranges[s_flash.num_protected++] = (range_t){ addr, len };
    return true;
}

void flash_emul_inject(flash_fault_t kind, uint32_t after_bytes)
{
    s_flash.fault = kind;
    s_flash.fault_budget = after_bytes;
}

void flash_emul_set_power_loss_handler(void (*handler)(void))
{
    s_flash.power_loss_handler = handler;
}

void flash_emul_power_on(void)
{
    s_flash.dead = false;
}

void flash_emul_get_stats(flash_stats_t *stats)
{
    *stats = s_flash.stats;
}

/* How many bytes of an operation of `len` bytes may run before the armed
 * fault fires: all of them if no fault of this kind is armed or it fires
 * later; fewer if it fires inside this operation. */
static uint32_t allowed_bytes(bool fault_applies, uint32_t len)
{
    if (!fault_applies) {
        return len;
    }
    if (s_flash.fault_budget < len) {
        return s_flash.fault_budget;
    }
    s_flash.fault_budget -= len;
    return len;
}

static flash_status_t fire_fault(void)
{
    const flash_fault_t kind = s_flash.fault;
    s_flash.fault = FLASH_FAULT_NONE;
    if (kind == FLASH_FAULT_PROGRAM_FAIL) {
        return FLASH_ERR_FAULT;
    }
    s_flash.dead = true;
    if (s_flash.power_loss_handler != NULL) {
        s_flash.power_loss_handler(); /* target: resets the CPU, does not return */
    }
    return FLASH_ERR_POWER_LOSS;
}

flash_status_t flash_erase(uint32_t addr, uint32_t len)
{
    if (s_flash.dead) {
        return FLASH_ERR_POWER_LOSS;
    }
    if (!in_range(addr, len)) {
        return FLASH_ERR_RANGE;
    }
    if ((addr - s_flash.base) % FLASH_PAGE_SIZE != 0u || len % FLASH_PAGE_SIZE != 0u) {
        return FLASH_ERR_ALIGN;
    }
    if (is_protected(addr, len)) {
        return FLASH_ERR_PROTECTED;
    }

    const uint32_t n = allowed_bytes(s_flash.fault == FLASH_FAULT_POWER_LOSS_ERASE, len);
    uint8_t *p = cell(addr);
    for (uint32_t i = 0; i < n; i++) {
        p[i] = 0xFFu;
    }
    s_flash.stats.pages_erased += n / FLASH_PAGE_SIZE;
    return (n < len) ? fire_fault() : FLASH_OK;
}

flash_status_t flash_program(uint32_t addr, const void *data, uint32_t len)
{
    if (s_flash.dead) {
        return FLASH_ERR_POWER_LOSS;
    }
    if (!in_range(addr, len)) {
        return FLASH_ERR_RANGE;
    }
    if ((addr - s_flash.base) % FLASH_WRITE_SIZE != 0u || len % FLASH_WRITE_SIZE != 0u) {
        return FLASH_ERR_ALIGN;
    }
    if (is_protected(addr, len)) {
        return FLASH_ERR_PROTECTED;
    }

    const uint8_t *src = (const uint8_t *)data;
    uint8_t *dst = cell(addr);
    /* Validate everything before changing anything, like a flash controller
     * that refuses the operation (PROGERR) rather than half-doing it. */
    for (uint32_t i = 0; i < len; i++) {
        if ((dst[i] & src[i]) != src[i]) {
            return FLASH_ERR_NOT_ERASED;
        }
    }

    const uint32_t n = allowed_bytes(s_flash.fault == FLASH_FAULT_PROGRAM_FAIL ||
                                         s_flash.fault == FLASH_FAULT_POWER_LOSS, len);
    for (uint32_t i = 0; i < n; i++) {
        dst[i] &= src[i]; /* programming can only clear bits */
    }
    s_flash.stats.bytes_programmed += n;
    return (n < len) ? fire_fault() : FLASH_OK;
}

const char *flash_status_name(flash_status_t s)
{
    switch (s) {
    case FLASH_OK:             return "ok";
    case FLASH_ERR_RANGE:      return "out of range";
    case FLASH_ERR_ALIGN:      return "misaligned";
    case FLASH_ERR_PROTECTED:  return "write-protected";
    case FLASH_ERR_NOT_ERASED: return "not erased";
    case FLASH_ERR_FAULT:      return "program failed (injected)";
    case FLASH_ERR_POWER_LOSS: return "power lost (injected)";
    default:                   return "?";
    }
}
