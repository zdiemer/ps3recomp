/*
 * ps3recomp - boot milestone log
 *
 * An always-on, cheap, deterministic record of what a recompiled title actually
 * did: which firmware imports it called, which lv2 syscalls it made, whether it
 * ever drew anything -- in first-occurrence order. `tools/regress.py` diffs that
 * record against a checked-in golden, which is how we find out that a change to
 * the shared runtime broke somebody else's port.
 *
 * Three properties it has to have, and why:
 *
 *  - It must not distort timing. Verbose tracing starves guest threads and
 *    changes behaviour, so a gate built on it would lie about the thing it is
 *    supposed to be checking. A repeat occurrence costs one string hash and
 *    one relaxed atomic add. First keys, scalar facts and count checkpoints
 *    enter a fixed bounded queue; a background writer owns all file operations.
 *    It checkpoints counts every couple of seconds on its own timer, even if
 *    no guest thread reaches another milestone.
 *
 *  - It must survive a hang. The interesting failure is a title that stops
 *    making progress, and a process killed on a timeout never reaches atexit().
 *    So first keys and scalar facts are appended by the writer as they arrive,
 *    and occurrence counts are re-appended every couple of seconds -- last
 *    block wins. Nothing here needs the clean exit, which matters because
 *    almost no gated title gets one. Queue saturation is reported in the log
 *    as an explicit overflow marker rather than silently dropping records.
 *
 *  - It must cost almost nothing when unused. With PS3_MILESTONE_OUT unset the
 *    entry points perform atomic state checks and return without creating a
 *    thread or touching a file, so it stays compiled into every port.
 *
 * Ports need no code of their own: the hooks live in the shared runtime, at the
 * choke points every title already funnels through (ps3_hle_call, lv2_syscall,
 * the RSX draw methods).
 *
 * Enable by setting PS3_MILESTONE_OUT to the output path.
 */

#ifndef PS3EMU_MILESTONE_H
#define PS3EMU_MILESTONE_H

#ifdef __cplusplus
extern "C" {
#endif

/* Record one occurrence of `key`.
 *
 * The first occurrence is assigned the next ordinal and queued for the log
 * writer; later ones only bump a counter. `key` is copied, so callers may pass
 * a stack buffer. The bounded queue reports any saturation in the output.
 *
 * Keys are namespaced by a prefix so the diff reads well and so regress.py can
 * group them: "hle:" firmware imports, "sys:" lv2 syscalls, "rsx:" draw paths,
 * "load:" loader events. */
void ps3_ms(const char* key);

/* Same, with a formatted key. Formats before it can know whether the key is
 * already known, so keep it off hot paths -- callers with a cheap "have I seen
 * this?" test of their own (an array indexed by syscall number, say) should use
 * that and call this only on the first sighting. */
void ps3_msf(const char* fmt, ...);

/* Record a scalar fact -- functions lifted, imports left unresolved. Queued
 * for the writer as it is set, so it survives a kill like the ordered stream
 * does; recording the same key again appends, and the reader takes the last
 * line for it. */
void ps3_ms_kv(const char* key, long long value);

/* Stop accepting records, let the writer drain, write final counts and close
 * the log. Registered with atexit() on first use; calling it directly is safe
 * and idempotent. Counts are also checkpointed as the run goes, so this is the
 * tidy ending, not the only one. */
void ps3_ms_dump(void);

#ifdef __cplusplus
} /* extern "C" */
#endif

#endif /* PS3EMU_MILESTONE_H */
