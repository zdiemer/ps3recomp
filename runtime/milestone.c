/*
 * ps3recomp - boot milestone log (see include/ps3emu/milestone.h)
 *
 * Guest threads publish bounded records to a fixed MPSC queue. A dedicated
 * writer owns all file operations; it never takes the producer/table lock.
 * This keeps slow checkpoint flushes from parking a PPU or SPU at a milestone.
 */
#include "ps3emu/milestone.h"
#include "platform/win32_compat.h"

#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#define MS_CAP             2048u
#define MS_KEY_MAX         96u
#define MS_RECORD_KEY_MAX  512u
#define MS_PATH_MAX        4096u
#define MS_QUEUE_CAP       4096u
#define MS_DRAIN_BATCH     256u
#define MS_CKPT_SECS       2

typedef struct {
    int state;                 /* 0 empty, 2 readable; state 1 is reserved */
    unsigned ord;              /* immutable after state=2 release */
    unsigned hits;             /* accessed only through __atomic builtins */
    char key[MS_KEY_MAX];
} ms_slot;

enum { MS_REC_FIRST=1, MS_REC_KV, MS_REC_COUNTS, MS_REC_WARNING };
typedef struct {
    uint32_t type;
    uint32_t ord;
    long long value;
    char key[MS_RECORD_KEY_MAX];
} ms_record;
typedef struct {
    uint64_t sequence;
    ms_record record;
} ms_cell;

static ms_slot g_slots[MS_CAP];
static int g_lock;
static unsigned g_next_ord;
static int g_full_warned;
static int g_enabled=-1;       /* -1 unresolved, 0 disabled/closed, 1 enabled */
static int g_closing;
static int g_dumped;

static ms_cell g_queue[MS_QUEUE_CAP];
static uint64_t g_enqueue_pos;
static uint64_t g_dequeue_pos; /* writer-owned */
static uint64_t g_emitted[MS_CAP+1];
static uint64_t g_dropped;
static int g_wake_pending;
static char g_path[MS_PATH_MAX];
static HANDLE g_writer_event;
static HANDLE g_writer_thread;
static FILE* g_out;            /* writer-owned after thread starts */
static int g_reported_io_error; /* writer-owned */
static int g_failed;

#ifdef PS3_MILESTONE_TEST_HOOKS
extern void ps3_ms_test_writer_gate(void);
extern void ps3_ms_test_record_written(unsigned type);
#endif

static void ms_lock(void)
{
    while (__atomic_exchange_n(&g_lock,1,__ATOMIC_ACQUIRE)) { }
}
static void ms_unlock(void) { __atomic_store_n(&g_lock,0,__ATOMIC_RELEASE); }

static unsigned ms_hash(const char* s)
{
    unsigned h=2166136261u;
    for (; *s; ++s) { h^=(unsigned char)*s; h*=16777619u; }
    return h;
}

static void ms_wake_writer(void)
{
    if (g_writer_event && !__atomic_exchange_n(&g_wake_pending,1,__ATOMIC_ACQ_REL))
        SetEvent(g_writer_event);
}

/* Bounded lock-free MPSC queue. The single writer releases cells after copying
 * each record, so producers never wait for formatting, disk IO, or fflush. */
static int ms_queue_push(const ms_record* record)
{
    uint64_t pos=__atomic_load_n(&g_enqueue_pos,__ATOMIC_RELAXED);
    for (;;) {
        ms_cell* cell=&g_queue[pos&(MS_QUEUE_CAP-1u)];
        uint64_t seq=__atomic_load_n(&cell->sequence,__ATOMIC_ACQUIRE);
        int64_t diff=(int64_t)(seq-pos);
        if (diff==0) {
            if (__atomic_compare_exchange_n(&g_enqueue_pos,&pos,pos+1,1,
                    __ATOMIC_RELAXED,__ATOMIC_RELAXED)) {
                cell->record=*record;
                __atomic_store_n(&cell->sequence,pos+1,__ATOMIC_RELEASE);
                ms_wake_writer();
                return 1;
            }
        } else if (diff<0) {
            __atomic_add_fetch(&g_dropped,1u,__ATOMIC_RELAXED);
            ms_wake_writer();
            return 0;
        } else {
            pos=__atomic_load_n(&g_enqueue_pos,__ATOMIC_RELAXED);
        }
    }
}

static int ms_queue_pop(ms_record* record)
{
    uint64_t pos=g_dequeue_pos;
    ms_cell* cell=&g_queue[pos&(MS_QUEUE_CAP-1u)];
    uint64_t seq=__atomic_load_n(&cell->sequence,__ATOMIC_ACQUIRE);
    if ((int64_t)(seq-(pos+1))!=0) return 0;
    *record=cell->record;
    __atomic_store_n(&cell->sequence,pos+MS_QUEUE_CAP,__ATOMIC_RELEASE);
    g_dequeue_pos=pos+1;
    return 1;
}

static int ms_queue_has_record(void)
{
    ms_cell* cell=&g_queue[g_dequeue_pos&(MS_QUEUE_CAP-1u)];
    uint64_t seq=__atomic_load_n(&cell->sequence,__ATOMIC_ACQUIRE);
    return (int64_t)(seq-(g_dequeue_pos+1))==0;
}

static const char* ms_bucket(unsigned n)
{
    if (!n) return "0";
    if (n==1) return "1";
    if (n<10) return "2-9";
    if (n<100) return "10-99";
    if (n<1000) return "100-999";
    if (n<10000) return "1k-9k";
    if (n<100000) return "10k-99k";
    return "100k+";
}

static void ms_flush_output(void)
{
    if (!g_out) return;
    if (fflush(g_out)!=0 || ferror(g_out)) {
        if (!g_reported_io_error) {
            g_reported_io_error=1;
            fprintf(stderr,"[milestone] output write failed; later records are unavailable\n");
        }
        __atomic_store_n(&g_failed,1,__ATOMIC_RELEASE);
        fclose(g_out);
        g_out=NULL;
    }
}

/* Called only by the writer. Keys and ordinals are immutable after release
 * publication; hits are atomic because guest calls continue while this runs. */
static void ms_write_counts(void)
{
    unsigned short by_ord[MS_CAP+1];
    unsigned i,ord;
    memset(by_ord,0,sizeof by_ord);
    if (!g_out) return;
    for (i=0;i<MS_CAP;i++) {
        ms_slot* s=&g_slots[i];
        if (__atomic_load_n(&s->state,__ATOMIC_ACQUIRE)==2 && s->ord>0 &&
            s->ord<=MS_CAP && __atomic_load_n(&g_emitted[s->ord],__ATOMIC_ACQUIRE))
            by_ord[s->ord]=(unsigned short)(i+1u);
    }
    fprintf(g_out,"# counts\n");
    for (ord=1;ord<=MS_CAP;ord++) {
        if (by_ord[ord]) {
            const ms_slot* s=&g_slots[by_ord[ord]-1];
            fprintf(g_out,"count\t%s\t%s\n",s->key,
                ms_bucket(__atomic_load_n(&s->hits,__ATOMIC_RELAXED)));
        }
    }
    ms_flush_output();
#ifdef PS3_MILESTONE_TEST_HOOKS
    if (g_out) ps3_ms_test_record_written(MS_REC_COUNTS);
#endif
}

static void ms_writer_write(const ms_record* r)
{
    if (r->type==MS_REC_WARNING) {
        fprintf(stderr,"%s\n",r->key);
        return;
    }
    if (!g_out) return;
    switch (r->type) {
    case MS_REC_FIRST:
        fprintf(g_out,"%u\t%s\n",r->ord,r->key);
        ms_flush_output();
        if (g_out && r->ord<=MS_CAP) __atomic_store_n(&g_emitted[r->ord],1u,__ATOMIC_RELEASE);
        break;
    case MS_REC_KV:
        fprintf(g_out,"kv\t%s\t%lld\n",r->key,r->value);
        ms_flush_output();
        break;
    case MS_REC_COUNTS: ms_write_counts(); break;
    case MS_REC_WARNING: break;
    }
#ifdef PS3_MILESTONE_TEST_HOOKS
    ps3_ms_test_record_written(r->type);
#endif
}

static DWORD ms_writer_main(LPVOID unused)
{
    ms_record r;
    uint64_t reported_drops=0;
    ULONGLONG next_checkpoint;
    (void)unused;
    g_out=fopen(g_path,"w");
    if (g_out) {
        fprintf(g_out,"# ps3recomp milestone log v1\n");
        ms_flush_output();
    } else {
        __atomic_store_n(&g_failed,1,__ATOMIC_RELEASE);
        fprintf(stderr,"[milestone] cannot open configured output -- log disabled\n");
    }
#ifdef PS3_MILESTONE_TEST_HOOKS
    ps3_ms_test_writer_gate();
#endif
    next_checkpoint=GetTickCount64()+(ULONGLONG)MS_CKPT_SECS*1000u;
    for (;;) {
        unsigned drained=0;
        while (drained<MS_DRAIN_BATCH && ms_queue_pop(&r)) {
            ms_writer_write(&r);
            ++drained;
        }

        /* Bound each drain so a steady producer stream cannot starve the
         * periodic checkpoint deadline. */
        ULONGLONG now=GetTickCount64();
        if (now>=next_checkpoint && g_out &&
            !__atomic_load_n(&g_closing,__ATOMIC_ACQUIRE)) {
            ms_write_counts();
            next_checkpoint=now+(ULONGLONG)MS_CKPT_SECS*1000u;
        }

        uint64_t dropped=__atomic_exchange_n(&g_dropped,0u,__ATOMIC_ACQ_REL);
        if (dropped) {
            reported_drops+=dropped;
            if (g_out) {
                fprintf(g_out,"# async-overflow records=%llu cumulative=%llu\n",
                    (unsigned long long)dropped,(unsigned long long)reported_drops);
                ms_flush_output();
            }
        }
        if (__atomic_load_n(&g_closing,__ATOMIC_ACQUIRE)) {
            /* Closing is published only after all producers holding g_lock have
             * left. No new records can be reserved after that publication. */
            if (!ms_queue_pop(&r)) break;
            ms_writer_write(&r);
            continue;
        }
        __atomic_store_n(&g_wake_pending,0,__ATOMIC_RELEASE);
        /* Close the drain-to-wait race: a producer that saw wake_pending=1
         * before the clear is caught by this recheck; later producers signal. */
        if (__atomic_load_n(&g_closing,__ATOMIC_ACQUIRE) || ms_queue_has_record() ||
            __atomic_load_n(&g_dropped,__ATOMIC_ACQUIRE))
            continue;
        now=GetTickCount64();
        /* Failed output has nothing left to checkpoint. Sleep until a raced
         * producer or shutdown wakes us instead of polling an expired timer. */
        DWORD timeout=!g_out ? INFINITE :
            (now>=next_checkpoint ? 0u : (DWORD)(next_checkpoint-now));
        WaitForSingleObject(g_writer_event,timeout);
        now=GetTickCount64();
        if (now>=next_checkpoint && g_out &&
            !__atomic_load_n(&g_closing,__ATOMIC_ACQUIRE)) {
            ms_write_counts();
            next_checkpoint=now+(ULONGLONG)MS_CKPT_SECS*1000u;
        }
    }
    if (g_out) {
        ms_write_counts();
        if (g_out) {
            fprintf(g_out,"# end\n");
            ms_flush_output();
        }
        if (g_out) {
            if (fclose(g_out)!=0) {
                __atomic_store_n(&g_failed,1,__ATOMIC_RELEASE);
                if (!g_reported_io_error)
                    fprintf(stderr,"[milestone] output close failed\n");
            }
            g_out=NULL;
        }
    }
    return 0;
}

static int ms_enabled(void)
{
    int e=__atomic_load_n(&g_enabled,__ATOMIC_ACQUIRE);
    if (e>=0) return e && !__atomic_load_n(&g_failed,__ATOMIC_ACQUIRE);
    ms_lock();
    e=__atomic_load_n(&g_enabled,__ATOMIC_RELAXED);
    if (e<0) {
        const char* path=getenv("PS3_MILESTONE_OUT");
        e=0;
        if (path && *path) {
            size_t n=strlen(path);
            if (n<sizeof g_path) {
                unsigned i;
                memcpy(g_path,path,n+1);
                for (i=0;i<MS_QUEUE_CAP;i++)
                    __atomic_store_n(&g_queue[i].sequence,(uint64_t)i,__ATOMIC_RELAXED);
                g_writer_event=CreateEventA(NULL,FALSE,FALSE,NULL);
                if (g_writer_event)
                    g_writer_thread=CreateThread(NULL,0,ms_writer_main,NULL,0,NULL);
                if (g_writer_thread) {
                    atexit(ps3_ms_dump);
                    e=1;
                } else {
                    if (g_writer_event) CloseHandle(g_writer_event);
                    g_writer_event=NULL;
                    fprintf(stderr,"[milestone] writer startup failed -- log disabled\n");
                }
            } else {
                fprintf(stderr,"[milestone] output path exceeds %u bytes -- log disabled\n",
                    (unsigned)(sizeof g_path-1));
            }
        }
        __atomic_store_n(&g_enabled,e,__ATOMIC_RELEASE);
    }
    ms_unlock();
    return e && !__atomic_load_n(&g_failed,__ATOMIC_ACQUIRE);
}

static void ms_insert(const char* key,unsigned h)
{
    unsigned i;
    ms_lock();
    if (__atomic_load_n(&g_closing,__ATOMIC_RELAXED) ||
        __atomic_load_n(&g_failed,__ATOMIC_ACQUIRE)) { ms_unlock(); return; }
    for (i=0;i<MS_CAP;i++) {
        ms_slot* s=&g_slots[(h+i)&(MS_CAP-1u)];
        if (__atomic_load_n(&s->state,__ATOMIC_RELAXED)==0) {
            ms_record r={0};
            snprintf(s->key,sizeof s->key,"%s",key);
            __atomic_store_n(&s->hits,1u,__ATOMIC_RELAXED);
            r.type=MS_REC_FIRST;
            r.ord=g_next_ord+1;
            snprintf(r.key,sizeof r.key,"%s",s->key);
            if (ms_queue_push(&r)) {
                s->ord=++g_next_ord;
            } else {
                s->ord=0; /* visible through the explicit queue-overflow marker */
            }
            __atomic_store_n(&s->state,2,__ATOMIC_RELEASE);
            ms_unlock();
            return;
        }
        if (__atomic_load_n(&s->state,__ATOMIC_RELAXED)==2 && strcmp(s->key,key)==0) {
            __atomic_fetch_add(&s->hits,1u,__ATOMIC_RELAXED);
            ms_unlock();
            return;
        }
    }
    if (!g_full_warned) {
        g_full_warned=1;
        ms_record r={0}; r.type=MS_REC_WARNING;
        snprintf(r.key,sizeof r.key,"[milestone] table full at %u keys -- dropping later keys; raise MS_CAP",MS_CAP);
        ms_queue_push(&r);
    }
    ms_unlock();
}

void ps3_ms(const char* key)
{
    unsigned h,i;
    if (!key || !*key || !ms_enabled()) return;
    h=ms_hash(key);
    for (i=0;i<MS_CAP;i++) {
        ms_slot* s=&g_slots[(h+i)&(MS_CAP-1u)];
        int st=__atomic_load_n(&s->state,__ATOMIC_ACQUIRE);
        if (st==0) break;
        if (st==2 && strcmp(s->key,key)==0) {
            __atomic_fetch_add(&s->hits,1u,__ATOMIC_RELAXED);
            return;
        }
    }
    ms_insert(key,h);
}

void ps3_msf(const char* fmt,...)
{
    char buf[MS_KEY_MAX];
    va_list ap;
    if (!ms_enabled()) return;
    va_start(ap,fmt); vsnprintf(buf,sizeof buf,fmt,ap); va_end(ap);
    ps3_ms(buf);
}

void ps3_ms_kv(const char* key,long long value)
{
    ms_record r={0};
    size_t n;
    if (!key || !*key || !ms_enabled()) return;
    n=strlen(key);
    r.type=MS_REC_KV; r.value=value;
    ms_lock();
    if (!__atomic_load_n(&g_closing,__ATOMIC_RELAXED) &&
        !__atomic_load_n(&g_failed,__ATOMIC_ACQUIRE)) {
        if (n>=sizeof r.key) {
            __atomic_add_fetch(&g_dropped,1u,__ATOMIC_RELAXED);
            ms_wake_writer();
        } else {
            memcpy(r.key,key,n+1);
            ms_queue_push(&r);
        }
    }
    ms_unlock();
}

void ps3_ms_dump(void)
{
    (void)ms_enabled();
    ms_lock();
    if (g_dumped || !g_writer_thread) { ms_unlock(); return; }
    g_dumped=1;
    __atomic_store_n(&g_closing,1,__ATOMIC_RELEASE);
    __atomic_store_n(&g_enabled,0,__ATOMIC_RELEASE);
    ms_unlock();
    ms_wake_writer();
    if (g_writer_thread) {
        WaitForSingleObject(g_writer_thread,INFINITE);
        CloseHandle(g_writer_thread); g_writer_thread=NULL;
    }
    if (g_writer_event) { CloseHandle(g_writer_event); g_writer_event=NULL; }
}
