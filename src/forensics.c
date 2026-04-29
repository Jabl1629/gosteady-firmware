/*
 * M10.7.3 — crash forensics + watchdog implementation.
 *
 * Layout on the `crash_forensics` flash partition (M10.7.1, 64 KB at
 * 0xcd2000 on external GD25LE255E):
 *
 *   block 0 (4 KB)   struct gosteady_forensics_record (single in-place slot)
 *   block 1..15      reserved for future per-fault history ring
 *
 * Update protocol for block 0: read entire 4 KB sector → modify in RAM →
 * erase sector → write back. The writes are infrequent (once per boot for
 * counter+reset-cause, plus once per fatal error inside the fault handler)
 * so the read-erase-write overhead is fine. Wear is bounded: at 1 boot/day
 * deployment cadence + GD25LE255E's 100k erase cycles, the block lives
 * ~270 years before wearing out. No wear-leveling needed.
 *
 * Magic + version handle field migration: a partition formatted before this
 * firmware (or wiped) reads as all-0xff which fails the magic check; we
 * zero the whole record and write a v1 entry, then proceed. Subsequent
 * boots hit the magic-match fast path.
 *
 * Fatal error handler integration: we override the Zephyr weak symbol
 * `k_sys_fatal_error_handler` so we get called on any fault before the
 * default reboot fires. Captures PC/LR/PSR + thread name into the record,
 * increments fault_count, persists, then forwards to the default handler
 * (which logs the frame on uart0 and resets).
 *
 * Watchdog: standard Zephyr WDT API on the nRF91 hardware watchdog (wdt0).
 * Single channel, ~60 s timeout. A dedicated supervisor thread kicks at
 * 20 s cadence. If the kernel hangs (any priority) for >60 s, the WDT
 * fires a hardware reset; next boot reads RESET_WATCHDOG via hwinfo and
 * increments watchdog_hits. No per-thread liveness check in v1 (would
 * require beacon coordination across sampler/writer/cloud workers); the
 * supervisor's existence is enough canary for the deployment-readiness
 * floor M10.7.3 needs to satisfy.
 */

#include "forensics.h"

#include <stdio.h>
#include <string.h>

#include <zephyr/kernel.h>
#include <zephyr/device.h>
#include <zephyr/drivers/watchdog.h>
#include <zephyr/drivers/hwinfo.h>
#include <zephyr/storage/flash_map.h>
#include <zephyr/logging/log.h>
#include <zephyr/sys/atomic.h>
#include <zephyr/sys/reboot.h>
#include <zephyr/fatal.h>
#include <zephyr/arch/exception.h>

LOG_MODULE_REGISTER(gs_forensics, LOG_LEVEL_INF);

#define FORENSICS_PARTITION_LABEL crash_forensics
#define FORENSICS_RECORD_OFFSET   0x0000
#define FORENSICS_BLOCK_SIZE      0x1000  /* GD25LE255E erase block */

#define GS_FORENSICS_MAGIC    0x53464F47u  /* 'GOFS' little-endian */
#define GS_FORENSICS_VERSION  1u

/* Persisted record (256 bytes; rest of the 4 KB block is unused for now).
 * Field offsets are part of the on-disk format — do not reorder or resize
 * without bumping VERSION. */
struct __attribute__((packed)) gs_forensics_fault {
	uint32_t pc;
	uint32_t lr;
	uint32_t psr;
	uint32_t reason;          /* K_ERR_* */
	uint64_t uptime_ms;
	char     thread_name[32];
	uint8_t  _pad[8];
};
_Static_assert(sizeof(struct gs_forensics_fault) == 64,
	"gs_forensics_fault must be 64 bytes");

struct __attribute__((packed)) gs_forensics_record {
	uint32_t magic;
	uint32_t version;
	uint32_t boot_count;
	uint32_t reset_reason;        /* hwinfo bitmask captured at THIS boot */
	uint32_t reset_reason_prev;   /* what was reset_reason at previous boot */
	uint32_t fault_count;
	uint32_t watchdog_hits;
	uint32_t assert_count;
	struct gs_forensics_fault last_fault;
	uint8_t  _reserved[256 - 32 - sizeof(struct gs_forensics_fault)];
};
_Static_assert(sizeof(struct gs_forensics_record) == 256,
	"gs_forensics_record must be 256 bytes");

static atomic_t s_initialized = ATOMIC_INIT(0);
static struct gs_forensics_record s_record;
static K_MUTEX_DEFINE(s_lock);

/* SRAM noinit retention.
 *
 * The fault path (k_sys_fatal_error_handler) can't reliably write to flash:
 * the SPI flash driver is stateful and after a panic LOG_PANIC mode locks
 * out the kernel scheduler, so the erase+write sequence either stalls or
 * is preempted by sys_reboot. Empirically observed on this nRF9151+TF-M
 * platform (M10.7.3 bench validation 2026-04-29).
 *
 * Workaround: the handler stamps the fault info into a noinit RAM region
 * (which survives sys_reboot's NVIC_SystemReset on Cortex-M), and the
 * next-boot init path drains it into the on-flash record where flash I/O
 * is fully ready.
 *
 * Magic gate: on a cold power-on, .noinit holds whatever happened to be
 * in the SRAM cells (typically random or 0xff). The magic word lets us
 * distinguish "valid pending state from a recent fault" from "random
 * cold-boot bits." Init clears the magic after consuming the pending state. */
#define GS_FORENSICS_PENDING_MAGIC  0xF1A56666u  /* "FAILS66" 'ish */

struct __attribute__((aligned(4))) gs_forensics_pending {
	uint32_t magic;
	uint32_t fault_inc;
	uint32_t reason;
	uint32_t pc;
	uint32_t lr;
	uint32_t psr;
	uint64_t uptime_ms;
	char     thread_name[32];
};
static __attribute__((section(".noinit"))) struct gs_forensics_pending s_pending;

/* ---- Flash R/W helpers ---- */

static int forensics_read(struct gs_forensics_record *out)
{
	const struct flash_area *fa;
	int ret = flash_area_open(FIXED_PARTITION_ID(FORENSICS_PARTITION_LABEL), &fa);
	if (ret) { return ret; }
	ret = flash_area_read(fa, FORENSICS_RECORD_OFFSET, out, sizeof(*out));
	flash_area_close(fa);
	return ret;
}

static int forensics_write_locked(const struct gs_forensics_record *r)
{
	const struct flash_area *fa;
	int ret = flash_area_open(FIXED_PARTITION_ID(FORENSICS_PARTITION_LABEL), &fa);
	if (ret) { return ret; }
	/* Erase the whole 4 KB block before re-writing (NOR flash semantics). */
	ret = flash_area_erase(fa, FORENSICS_RECORD_OFFSET, FORENSICS_BLOCK_SIZE);
	if (ret) { flash_area_close(fa); return ret; }
	ret = flash_area_write(fa, FORENSICS_RECORD_OFFSET, r, sizeof(*r));
	flash_area_close(fa);
	return ret;
}

/* ---- Reset-reason formatting ---- */

static void format_reset_reason(uint32_t cause, char *out, size_t out_sz)
{
	if (out_sz == 0) { return; }
	out[0] = '\0';
	if (cause == 0) {
		strncpy(out, "POWER_ON", out_sz - 1);
		out[out_sz - 1] = '\0';
		return;
	}

	struct {
		uint32_t bit;
		const char *name;
	} table[] = {
		{ RESET_PIN,           "PIN" },
		{ RESET_SOFTWARE,      "SOFTWARE" },
		{ RESET_BROWNOUT,      "BROWNOUT" },
		{ RESET_POR,           "POR" },
		{ RESET_WATCHDOG,      "WATCHDOG" },
		{ RESET_DEBUG,         "DEBUG" },
		{ RESET_SECURITY,      "SECURITY" },
		{ RESET_LOW_POWER_WAKE,"LP_WAKE" },
		{ RESET_CPU_LOCKUP,    "CPU_LOCKUP" },
		{ RESET_PARITY,        "PARITY" },
		{ RESET_PLL,           "PLL" },
		{ RESET_CLOCK,         "CLOCK" },
	};

	size_t n = 0;
	for (size_t i = 0; i < ARRAY_SIZE(table); i++) {
		if (cause & table[i].bit) {
			int w = snprintf(out + n, out_sz - n, "%s%s",
					 (n > 0) ? "," : "", table[i].name);
			if (w < 0 || (size_t)w >= out_sz - n) { break; }
			n += (size_t)w;
		}
	}
	if (n == 0) {
		(void)snprintf(out, out_sz, "UNKNOWN_0x%08x", (unsigned)cause);
	}
}

/* ---- Public API ---- */

int gosteady_forensics_get_reset_reason(char *out, size_t out_sz)
{
	if (!out || out_sz < 32) { return -EINVAL; }
	uint32_t cause = atomic_get(&s_initialized) ? s_record.reset_reason : 0;
	format_reset_reason(cause, out, out_sz);
	return 0;
}

uint32_t gosteady_forensics_get_fault_count(void)
{
	return atomic_get(&s_initialized) ? s_record.fault_count : 0;
}

uint32_t gosteady_forensics_get_watchdog_hits(void)
{
	return atomic_get(&s_initialized) ? s_record.watchdog_hits : 0;
}

uint32_t gosteady_forensics_get_assert_count(void)
{
	return atomic_get(&s_initialized) ? s_record.assert_count : 0;
}

uint32_t gosteady_forensics_get_uptime_s(void)
{
	return (uint32_t)(k_uptime_get() / 1000);
}

int gosteady_forensics_fault_counters_json(char *buf, size_t buflen,
					   size_t *out_len)
{
	int n = snprintf(buf, buflen,
			 "{\"fatal\":%u,\"asserts\":%u,\"watchdog\":%u}",
			 (unsigned)gosteady_forensics_get_fault_count(),
			 (unsigned)gosteady_forensics_get_assert_count(),
			 (unsigned)gosteady_forensics_get_watchdog_hits());
	if (n < 0 || (size_t)n >= buflen) { return -ENOMEM; }
	if (out_len) { *out_len = (size_t)n; }
	return 0;
}

/* ---- Fault-handler override ----
 *
 * This overrides the Zephyr weak `k_sys_fatal_error_handler`. Called from
 * arch fault path with reason = K_ERR_* and esf = the captured exception
 * stack frame. We persist the relevant fields (PC/LR/PSR + thread name +
 * uptime + reason + fault counter increment) before forwarding to the
 * default handler, which logs and reboots.
 *
 * Constraints: the architecture has restricted runtime guarantees here —
 * we may be in interrupt context; some kernel objects may be in a partial
 * state. flash_area_open + erase + write IS callable here on Zephyr per
 * the flash subsystem contract (the underlying drivers are reentrant), but
 * we keep the path narrow:
 *   - copy ESF fields into the in-RAM s_record snapshot
 *   - write to flash without taking s_lock (mutex semantics aren't
 *     guaranteed in fault context; the system is going down anyway)
 *   - never log via LOG_* (calls back into kernel; we use printk only)
 *
 * If the fault is itself in the flash driver, the second flash op may
 * fail or wedge — we accept that and the post-mortem just won't have
 * THIS fault frame; previous-boot frame survives.
 */
void k_sys_fatal_error_handler(unsigned int reason, const struct arch_esf *esf)
{
	/* Stamp the fault info into noinit RAM. We do NOT touch flash here —
	 * empirically, post-LOG_PANIC + sys_reboot leaves no usable window
	 * for flash_area_erase + write to complete. Next-boot init drains
	 * this region into the on-flash record. */
	if (s_pending.magic != GS_FORENSICS_PENDING_MAGIC) {
		/* First fault since init cleared the slot — start fresh. */
		s_pending.fault_inc = 0;
	}
	s_pending.magic     = GS_FORENSICS_PENDING_MAGIC;
	s_pending.fault_inc++;
	s_pending.reason    = reason;
	s_pending.uptime_ms = k_uptime_get();
	if (esf) {
		s_pending.pc  = esf->basic.pc;
		s_pending.lr  = esf->basic.lr;
		s_pending.psr = esf->basic.xpsr;
	} else {
		s_pending.pc = s_pending.lr = s_pending.psr = 0;
	}

	const char *tname = k_thread_name_get(k_current_get());
	memset(s_pending.thread_name, 0, sizeof(s_pending.thread_name));
	if (tname) {
		strncpy(s_pending.thread_name, tname,
			sizeof(s_pending.thread_name) - 1);
	}

	printk("GoSteady forensics: captured fault PC=0x%08x reason=%u (pending in noinit)\n",
	       (unsigned)s_pending.pc, reason);

	/* Trigger a SoC reset. sys_reboot() routes through arch's
	 * NVIC_SystemReset on Cortex-M which produces a clean SOFTWARE reset
	 * (RESET_SOFTWARE bit in hwinfo on next boot) AND retains SRAM, so
	 * s_pending survives into next-boot init. Without this we'd hang in
	 * a halt loop until the WDT fires 60 s later. */
	sys_reboot(SYS_REBOOT_WARM);
	k_fatal_halt(reason);
}

/* ---- Watchdog supervisor ---- */

#define WDT_NODE DT_NODELABEL(wdt0)
#if !DT_NODE_HAS_STATUS(WDT_NODE, okay)
#error "wdt0 node not enabled"
#endif

static const struct device *const s_wdt = DEVICE_DT_GET(WDT_NODE);
static int s_wdt_channel = -1;

#define WDT_TIMEOUT_MS  60000  /* 60 s */
#define WDT_KICK_MS     20000  /* kick every 20 s — leaves 40 s margin */

K_THREAD_STACK_DEFINE(s_wdt_stack, 1024);
static struct k_thread s_wdt_thread;

#if defined(CONFIG_GOSTEADY_FORENSICS_STRESS)
static atomic_t s_stress_stall = ATOMIC_INIT(0);
#endif

static void wdt_supervisor_fn(void *p1, void *p2, void *p3)
{
	ARG_UNUSED(p1); ARG_UNUSED(p2); ARG_UNUSED(p3);
	while (1) {
#if defined(CONFIG_GOSTEADY_FORENSICS_STRESS)
		if (atomic_get(&s_stress_stall)) {
			/* Stall: skip wdt_feed and drop into a non-yielding
			 * busy loop so ALL paths to wdt_feed are blocked.
			 * 60 s later the hardware WDT fires a SoC reset. */
			LOG_WRN("stress: WDT stall engaged — expecting reset in ~60 s");
			while (1) { /* busy wait */ }
		}
#endif
		if (s_wdt_channel >= 0) {
			(void)wdt_feed(s_wdt, s_wdt_channel);
		}
		k_msleep(WDT_KICK_MS);
	}
}

#if defined(CONFIG_GOSTEADY_FORENSICS_STRESS)
__attribute__((noreturn)) void gosteady_forensics_stress_fault(void)
{
	LOG_WRN("stress: calling k_panic() in 100 ms — expect handler + reboot");
	k_msleep(100);
	/* k_panic() routes through Zephyr's arch fault path with reason
	 * K_ERR_KERNEL_PANIC, which lands in our k_sys_fatal_error_handler
	 * override. We use this rather than CPU fault hardware (NULL-deref,
	 * udf, divide-by-zero) because on the Thingy:91 X non-secure split
	 * with TF-M, all CPU faults are intercepted by the secure side and
	 * recovered only via watchdog timeout — the non-secure handler we
	 * own is not invoked. k_panic() bypasses TF-M because it's a software
	 * panic, not a hardware fault. */
	k_panic();
	while (1) { /* unreachable hint */ }
}

void gosteady_forensics_stress_stall_wdt(void)
{
	atomic_set(&s_stress_stall, 1);
}
#endif /* CONFIG_GOSTEADY_FORENSICS_STRESS */

static int watchdog_start(void)
{
	if (!device_is_ready(s_wdt)) {
		LOG_ERR("wdt device not ready");
		return -ENODEV;
	}

	struct wdt_timeout_cfg cfg = {
		.window  = { .min = 0, .max = WDT_TIMEOUT_MS },
		.callback = NULL,
		.flags   = WDT_FLAG_RESET_SOC,
	};
	s_wdt_channel = wdt_install_timeout(s_wdt, &cfg);
	if (s_wdt_channel < 0) {
		LOG_ERR("wdt_install_timeout failed (%d)", s_wdt_channel);
		return s_wdt_channel;
	}

	int ret = wdt_setup(s_wdt, WDT_OPT_PAUSE_HALTED_BY_DBG);
	if (ret) {
		LOG_ERR("wdt_setup failed (%d)", ret);
		return ret;
	}

	(void)k_thread_create(&s_wdt_thread, s_wdt_stack,
			      K_THREAD_STACK_SIZEOF(s_wdt_stack),
			      wdt_supervisor_fn, NULL, NULL, NULL,
			      9, 0, K_NO_WAIT);
	k_thread_name_set(&s_wdt_thread, "gs_wdt");
	LOG_INF("watchdog started: timeout=%d ms, kick=%d ms",
		WDT_TIMEOUT_MS, WDT_KICK_MS);
	return 0;
}

/* ---- Init ---- */

int gosteady_forensics_init(void)
{
	if (atomic_set(&s_initialized, 1) == 1) {
		return 0;
	}

	uint32_t cause = 0;
	int hwret = hwinfo_get_reset_cause(&cause);
	(void)hwinfo_clear_reset_cause();
	if (hwret == 0) {
		LOG_INF("hwinfo reset_cause=0x%08x", (unsigned)cause);
	} else {
		LOG_WRN("hwinfo_get_reset_cause failed (%d)", hwret);
	}

	int rret = forensics_read(&s_record);
	if (rret == 0 && s_record.magic == GS_FORENSICS_MAGIC &&
	    s_record.version == GS_FORENSICS_VERSION) {
		/* Existing record. Bump boot_count, slide reset_reason. */
		s_record.reset_reason_prev = s_record.reset_reason;
		s_record.reset_reason      = cause;
		s_record.boot_count++;
		if (cause & RESET_WATCHDOG) {
			s_record.watchdog_hits++;
			LOG_WRN("previous reset was WATCHDOG — count now %u",
				(unsigned)s_record.watchdog_hits);
		}
		/* Drain noinit pending state from the previous boot's fault
		 * handler. Magic match means we have valid pending fault info;
		 * ingest into the persistent record + clear so we don't double-
		 * count on a subsequent re-init that doesn't trip a fault. */
		if (s_pending.magic == GS_FORENSICS_PENDING_MAGIC &&
		    s_pending.fault_inc > 0) {
			s_record.fault_count += s_pending.fault_inc;
			s_record.last_fault.pc        = s_pending.pc;
			s_record.last_fault.lr        = s_pending.lr;
			s_record.last_fault.psr       = s_pending.psr;
			s_record.last_fault.reason    = s_pending.reason;
			s_record.last_fault.uptime_ms = s_pending.uptime_ms;
			memset(s_record.last_fault.thread_name, 0,
			       sizeof(s_record.last_fault.thread_name));
			memcpy(s_record.last_fault.thread_name,
			       s_pending.thread_name,
			       sizeof(s_record.last_fault.thread_name));
			LOG_WRN("drained noinit fault: +%u faults, last PC=0x%08x reason=%u",
				(unsigned)s_pending.fault_inc,
				(unsigned)s_pending.pc,
				(unsigned)s_pending.reason);
			s_pending.magic = 0;
			s_pending.fault_inc = 0;
		}
	} else {
		/* Fresh / wiped / corrupted partition: zero and v1-write. */
		LOG_INF("forensics: fresh init (read=%d magic=0x%08x version=%u)",
			rret, (unsigned)s_record.magic, (unsigned)s_record.version);
		memset(&s_record, 0, sizeof(s_record));
		s_record.magic        = GS_FORENSICS_MAGIC;
		s_record.version      = GS_FORENSICS_VERSION;
		s_record.boot_count   = 1;
		s_record.reset_reason = cause;
	}

	int wret = forensics_write_locked(&s_record);
	if (wret) {
		LOG_ERR("forensics_write failed (%d) — record not persisted",
			wret);
		/* Keep s_initialized=1 so getters still work with in-RAM
		 * record; just no persistence on this boot. */
	}

	char rs[64];
	format_reset_reason(s_record.reset_reason, rs, sizeof(rs));
	LOG_INF("forensics: boot=%u reset=%s faults=%u wdt=%u",
		(unsigned)s_record.boot_count, rs,
		(unsigned)s_record.fault_count,
		(unsigned)s_record.watchdog_hits);

	(void)watchdog_start();

	return 0;
}
