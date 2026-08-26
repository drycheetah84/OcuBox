#include "devices/irq.h"

namespace hw::dev {

static void (*s_raise)(uint32_t, bool) = nullptr;
static void (*s_enable)(uint32_t, bool) = nullptr;

void install_irq_backend(void (*raise)(uint32_t, bool), void (*enable)(uint32_t, bool)) {
    s_raise = raise; s_enable = enable;
}
void raise_irq(uint32_t intid, bool level)       { if (s_raise)  s_raise(intid, level); }
void set_irq_enabled(uint32_t intid, bool enabled){ if (s_enable) s_enable(intid, enabled); }

} // namespace hw::dev
