#ifndef RINGBUF_H
#define RINGBUF_H

/*
 * ============================================================================
 *  ringbuf.h — Generic SPSC ring buffer primitive (kernel-internal)
 * ----------------------------------------------------------------------------
 *  Equinox OS v10.5. One primitive, three instances:
 *
 *      kbd_ring   (idt.cpp)        scancodes,  producer = IRQ1,  consumer = task
 *      mouse_ring (ps2_mouse.cpp)  packets,    producer = IRQ12, consumer = task
 *      audio_ring (libaudio.cpp)   notes,      producer = task,  consumer = IRQ0
 *
 *  DESIGN (modeled after Linux kfifo):
 *   - Capacity N must be a power of two (static_assert): index masking
 *     with & (N-1) replaces the expensive % modulo of the old
 *     hand-rolled kbd_buffer, and lets head/tail be MONOTONIC u32
 *     counters that never need wrap-around special cases.
 *   - count = head - tail (unsigned arithmetic, wrap-safe): no wasted
 *     slot, unlike the old "next_head != tail" scheme which could only
 *     ever store SIZE-1 items.
 *   - Single-Producer / Single-Consumer on a single CPU. The direction
 *     does NOT matter: for kbd/mouse the producer is an IRQ and the
 *     consumer is task code, for audio it is the other way around
 *     (syscall pushes, timer IRQ pops). EVERY mutating operation
 *     (push/push_overwrite/pop/reset) wraps itself in irq_save/cli/
 *     irq_restore, so an operation is atomic regardless of which side
 *     calls it:
 *       * called from task context (IF=1): the cli suppresses the
 *         interrupt on the other side for the duration;
 *       * called from IRQ context (IF=0): save/restore is a no-op.
 *     Nested guards are safe because flags are saved and restored, not
 *     blindly re-enabled (the same discipline as ps2_mouse.cpp).
 *     Cost: ~20 cycles of pushfl/popfl per op — irrelevant next to any
 *     IRQ rate this kernel drives (worst case: timer at 100 Hz).
 *   - Lock-free readers: count()/empty()/drops() take no guard. They
 *     may be stale by one item if an interrupt lands mid-expression.
 *     That is fine for every current use (poll-then-act loops always
 *     re-check; ringstats is diagnostics). Documented per-method.
 *
 *  OVERFLOW POLICIES (deliberately two):
 *   - push()           : drop-NEWEST, returns false. For ordered streams
 *                        (keyboard): typing order must never scramble,
 *                        same choice Linux makes for tty input.
 *   - push_overwrite() : drop-OLDEST, always stores. For motion streams
 *                        (mouse): when a consumer stalls, the freshest
 *                        deltas matter more than the old ones.
 *   Both count the loss in `dropped_` — ringstats surfaces it.
 *
 *  MEMORY: instances are plain static objects (.bss), no malloc, no
 *  constructor side effects — safe before kernel_main and from any
 *  interrupt context after IDT load. T must be trivially copyable.
 * ============================================================================
 */

#include <stdint.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ---- Interrupt-flag save/restore (the only "lock" we have) -----------
 * Identical to the irq_save/irq_restore discipline in ps2_mouse.cpp,
 * lifted here so the ring primitive is self-contained. NEVER use bare
 * cli/sti pairs: restoring IF unconditionally would re-enable interrupts
 * inside an interrupt handler that entered with IF=0. */
static inline uint32_t rb_irq_save(void) {
    uint32_t flags;
    asm volatile("pushfl\n\tpopl %0\n\tcli" : "=r"(flags) :: "memory");
    return flags;
}

static inline void rb_irq_restore(uint32_t flags) {
    asm volatile("pushl %0\n\tpopfl" :: "r"(flags) : "memory");
}

#ifdef __cplusplus
} /* extern "C" */

template <typename T, unsigned N>
class RingBuffer {
    static_assert(N > 1 && (N & (N - 1)) == 0,
                  "RingBuffer capacity must be a power of two (>= 2)");
public:
    /* Drop everything pending and zero the drop counter. Safe from any
     * context. Does NOT touch whatever the consumer already popped. */
    void reset(void) {
        uint32_t f = rb_irq_save();
        head_ = 0;
        tail_ = 0;
        dropped_ = 0;
        rb_irq_restore(f);
    }

    /* Store one item. If the ring is full the NEWEST item is dropped
     * (nothing is stored) and the drop counter increments. Returns
     * true when the item was stored. */
    bool push(const T& v) {
        uint32_t f = rb_irq_save();
        uint32_t head = head_;
        if (head - tail_ == N) {          /* full: keep old, drop new */
            dropped_++;
            rb_irq_restore(f);
            return false;
        }
        buf_[head & MASK] = v;
        head_ = head + 1;
        rb_irq_restore(f);
        return true;
    }

    /* Store one item, overwriting the OLDEST item when full. Returns
     * true when stored without a loss, false when the oldest item was
     * sacrificed (the new item is still stored). */
    bool push_overwrite(const T& v) {
        bool lost = false;
        uint32_t f = rb_irq_save();
        if (head_ - tail_ == N) {          /* full: evict the oldest */
            tail_++;
            dropped_++;
            lost = true;
        }
        buf_[head_ & MASK] = v;
        head_++;
        rb_irq_restore(f);
        return !lost;
    }

    /* Consume the oldest item into *out. Returns false when empty. */
    bool pop(T* out) {
        if (!out) return false;
        uint32_t f = rb_irq_save();
        uint32_t tail = tail_;
        if (head_ == tail) {
            rb_irq_restore(f);
            return false;
        }
        *out = buf_[tail & MASK];
        tail_ = tail + 1;
        rb_irq_restore(f);
        return true;
    }

    /* Look at the oldest item without consuming it. Returns false when
     * empty. Like pop(), guarded — peek-then-pop pairs stay coherent. */
    bool peek(T* out) const {
        if (!out) return false;
        uint32_t f = rb_irq_save();
        uint32_t tail = tail_;
        bool ok = head_ != tail;
        if (ok) *out = buf_[tail & MASK];
        rb_irq_restore(f);
        return ok;
    }

    /* ---- Lock-free observers ----------------------------------------
     * May race with the "other side" by one item (see file header).
     * Busy-wait consumers must re-check via the mutating calls. */
    uint32_t count(void)    const { return head_ - tail_; }
    uint32_t capacity(void) const { return N; }
    uint32_t drops(void)    const { return dropped_; }
    bool     empty(void)    const { return head_ == tail_; }
    bool     full(void)     const { return head_ - tail_ == N; }

private:
    static const uint32_t MASK = N - 1;

    T buf_[N];
    volatile uint32_t head_ = 0;   /* producer advances (monotonic) */
    volatile uint32_t tail_ = 0;   /* consumer advances (monotonic) */
    uint32_t dropped_ = 0;         /* items lost to overflow        */
};

#endif /* __cplusplus */

#endif /* RINGBUF_H */
