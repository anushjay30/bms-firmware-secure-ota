#include "health.h"

void health_init(health_t *h)
{
    h->healthy = false;
    h->since_ms = 0u;
}

bool health_update(health_t *h, uint32_t now_ms, bool tasks_alive, bool fault_free, uint32_t required_ms)
{
    if (!tasks_alive || !fault_free) {
        h->healthy = false;
        return false;
    }
    if (!h->healthy) {
        h->healthy = true;
        h->since_ms = now_ms;
    }
    if ((uint32_t)(now_ms - h->since_ms) >= required_ms) { /* wrap-safe */
        h->since_ms = now_ms;
        return true;
    }
    return false;
}

uint32_t health_healthy_ms(const health_t *h, uint32_t now_ms)
{
    return h->healthy ? (uint32_t)(now_ms - h->since_ms) : 0u;
}
