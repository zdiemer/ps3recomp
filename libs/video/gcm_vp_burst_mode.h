#ifndef GCM_VP_BURST_MODE_H
#define GCM_VP_BURST_MODE_H
/* Internal experiment control. Called only with the FIFO consumer lock held.
 * No user graphics setting; default off pending whole-frame validation. */
static struct {
    int initialized, enabled, reported, diagnostics, profile, active;
    int final_reported;
    const char* file;
    uint64_t next_poll, next_report, original, native, rejected, ticks;
} s_vp_burst_mode;

static uint64_t gcm_vp_burst_clock(void)
{
#ifdef _WIN32
    LARGE_INTEGER t; QueryPerformanceCounter(&t); return (uint64_t)t.QuadPart;
#else
    return 0;
#endif
}
static void gcm_vp_burst_counts(uint64_t qpc)
{
    fprintf(stderr,"[rsx-vp-burst-counts] qpc=%llu enabled=%d original=%llu native=%llu rejected=%llu ticks=%llu\n",
        (unsigned long long)qpc,s_vp_burst_mode.reported,
        (unsigned long long)s_vp_burst_mode.original,(unsigned long long)s_vp_burst_mode.native,
        (unsigned long long)s_vp_burst_mode.rejected,(unsigned long long)s_vp_burst_mode.ticks);
}
static void gcm_vp_burst_poll(void)
{
    if (!s_vp_burst_mode.initialized) {
        const char* e=getenv("RSX_VP_BURST");
        s_vp_burst_mode.enabled=e && !strcmp(e,"1");
        s_vp_burst_mode.file=getenv("RSX_VP_BURST_FILE");
        s_vp_burst_mode.profile=getenv("RSX_VP_BURST_PROFILE")!=NULL;
        s_vp_burst_mode.active=s_vp_burst_mode.enabled || s_vp_burst_mode.file || s_vp_burst_mode.profile;
        /* Keep per-word diagnostic counts/read side effects exact. */
        const char* names[]={"RSX_TRACE","GCM_VAL_WATCH","GCM_SUBCH_GATE","RSX_LIVE_FEED_DBG",
            "PPU_MEMORY_DIAGNOSTICS","PPU_HOTMAP","PPU_RWATCH","PPU_FORCE_READ_ADDR",
            "PPU_RVAL","PPU_RVAL64","PPU_SPINBT","PPU_WVAL","PPU_WWATCH",
            "PPU_WW_GUARD","PT","YDKJ_RWATCH","PS3_NULL_READ","PS3_NULL_SWEEP","PS3_NULL_WRITE"};
        for (unsigned i=0;i<sizeof(names)/sizeof(names[0]);++i)
            if (getenv(names[i])) s_vp_burst_mode.diagnostics=1;
#ifdef VM_SAMPLE_READS
        s_vp_burst_mode.diagnostics=1;
#endif
        s_vp_burst_mode.reported=-1;
        s_vp_burst_mode.initialized=1;
    }
#ifdef _WIN32
    if (!s_vp_burst_mode.active) return;
    /* The fixed RSX_VP_BURST=1 experiment has no changing control source and
     * does not need a periodic summary. Keep startup mode evidence but avoid
     * the poll clock and all periodic output in the ordinary fixed mode. */
    const int periodic_reports=s_vp_burst_mode.file || s_vp_burst_mode.profile;
    uint64_t now=0;
    if (periodic_reports) {
        now=GetTickCount64();
        if (s_vp_burst_mode.file && now>=s_vp_burst_mode.next_poll) {
            s_vp_burst_mode.next_poll=now+100;
            FILE* f=fopen(s_vp_burst_mode.file,"r"); unsigned value;
            if(f) { if(fscanf(f,"%u",&value)==1 && value<=1) s_vp_burst_mode.enabled=(int)value; fclose(f); }
        }
    }
    if (s_vp_burst_mode.enabled!=s_vp_burst_mode.reported) {
        const uint64_t qpc=gcm_vp_burst_clock();
        gcm_vp_burst_counts(qpc);
        s_vp_burst_mode.reported=s_vp_burst_mode.enabled;
        s_vp_burst_mode.original=s_vp_burst_mode.native=s_vp_burst_mode.rejected=s_vp_burst_mode.ticks=0;
        fprintf(stderr,"[rsx-vp-burst-mode] enabled=%d qpc=%llu\n",
            s_vp_burst_mode.enabled,(unsigned long long)qpc);
        fprintf(stderr,"[rsx-vp-burst] diagnostics=%d profile=%d\n",
            s_vp_burst_mode.diagnostics,s_vp_burst_mode.profile);
        if (periodic_reports) s_vp_burst_mode.next_report=now+2000;
    } else if(periodic_reports && now>=s_vp_burst_mode.next_report) {
        gcm_vp_burst_counts(gcm_vp_burst_clock());
        s_vp_burst_mode.next_report=now+2000;
    }
#else
    s_vp_burst_mode.enabled=0;
#endif
}
static void gcm_vp_burst_record(uint64_t started,int native)
{
    if (!s_vp_burst_mode.active) return;
    if (native) ++s_vp_burst_mode.native;
    else { ++s_vp_burst_mode.original; if(s_vp_burst_mode.enabled) ++s_vp_burst_mode.rejected; }
    if (started) s_vp_burst_mode.ticks+=gcm_vp_burst_clock()-started;
}

/* Called by the orderly-shutdown export while the FIFO lock is held. */
static void gcm_vp_burst_final_report(uint64_t qpc)
{
    if (!s_vp_burst_mode.initialized || !s_vp_burst_mode.active ||
        s_vp_burst_mode.final_reported)
        return;
    fprintf(stderr,
        "[rsx-vp-burst-final] qpc=%llu enabled=%d original=%llu native=%llu "
        "rejected=%llu ticks=%llu\n",
        (unsigned long long)qpc, s_vp_burst_mode.enabled,
        (unsigned long long)s_vp_burst_mode.original,
        (unsigned long long)s_vp_burst_mode.native,
        (unsigned long long)s_vp_burst_mode.rejected,
        (unsigned long long)s_vp_burst_mode.ticks);
    fflush(stderr);
    s_vp_burst_mode.final_reported=1;
}
#endif
