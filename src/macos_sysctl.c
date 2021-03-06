#include "common.h"
#include <sys/sysctl.h>
// NEEDED BY: do_bandwidth
#include <net/route.h>

#define GETSYSCTL(name, var) getsysctl(name, &(var), sizeof(var))

// MacOS calculates load averages once every 5 seconds
#define MIN_LOADAVG_UPDATE_EVERY 5

int getsysctl(const char *name, void *ptr, size_t len);

int do_macos_sysctl(int update_every, usec_t dt) {
    (void)dt;

    static int do_loadavg = -1, do_swap = -1, do_bandwidth = -1;

    if (unlikely(do_loadavg == -1)) {
        do_loadavg              = config_get_boolean("plugin:macos:sysctl", "enable load average", 1);
        do_swap                 = config_get_boolean("plugin:macos:sysctl", "system swap", 1);
        do_bandwidth            = config_get_boolean("plugin:macos:sysctl", "bandwidth", 1);
    }

    RRDSET *st;

    int system_pagesize = getpagesize(); // wouldn't it be better to get value directly from hw.pagesize?
    int i, n;
    int common_error = 0;
    size_t size;

    // NEEDED BY: do_loadavg
    static usec_t last_loadavg_usec = 0;
    struct loadavg sysload;

    // NEEDED BY: do_swap
    struct xsw_usage swap_usage;

    // NEEDED BY: do_bandwidth
    int mib[6];
    static char *ifstatdata = NULL;
    char *lim, *next;
    struct if_msghdr *ifm;
    struct iftot {
        u_long  ift_ibytes;
        u_long  ift_obytes;
    } iftot = {0, 0};

    if (last_loadavg_usec <= dt) {
        if (likely(do_loadavg)) {
            if (unlikely(GETSYSCTL("vm.loadavg", sysload))) {
                do_loadavg = 0;
                error("DISABLED: system.load");
            } else {

                st = rrdset_find_bytype("system", "load");
                if (unlikely(!st)) {
                    st = rrdset_create("system", "load", NULL, "load", NULL, "System Load Average", "load", 100, (update_every < MIN_LOADAVG_UPDATE_EVERY) ? MIN_LOADAVG_UPDATE_EVERY : update_every, RRDSET_TYPE_LINE);
                    rrddim_add(st, "load1", NULL, 1, 1000, RRDDIM_ABSOLUTE);
                    rrddim_add(st, "load5", NULL, 1, 1000, RRDDIM_ABSOLUTE);
                    rrddim_add(st, "load15", NULL, 1, 1000, RRDDIM_ABSOLUTE);
                }
                else rrdset_next(st);

                rrddim_set(st, "load1", (collected_number) ((double)sysload.ldavg[0] / sysload.fscale * 1000));
                rrddim_set(st, "load5", (collected_number) ((double)sysload.ldavg[1] / sysload.fscale * 1000));
                rrddim_set(st, "load15", (collected_number) ((double)sysload.ldavg[2] / sysload.fscale * 1000));
                rrdset_done(st);
            }
        }

        last_loadavg_usec = st->update_every * USEC_PER_SEC;
    }
    else last_loadavg_usec -= dt;

    if (likely(do_swap)) {
        if (unlikely(GETSYSCTL("vm.swapusage", swap_usage))) {
            do_swap = 0;
            error("DISABLED: system.swap");
        } else {
            st = rrdset_find("system.swap");
            if (unlikely(!st)) {
                st = rrdset_create("system", "swap", NULL, "swap", NULL, "System Swap", "MB", 201, update_every, RRDSET_TYPE_STACKED);
                st->isdetail = 1;

                rrddim_add(st, "free",    NULL, 1, 1048576, RRDDIM_ABSOLUTE);
                rrddim_add(st, "used",    NULL, 1, 1048576, RRDDIM_ABSOLUTE);
            }
            else rrdset_next(st);

            rrddim_set(st, "free", swap_usage.xsu_avail);
            rrddim_set(st, "used", swap_usage.xsu_used);
            rrdset_done(st);
        }
    }

    // --------------------------------------------------------------------

    if (likely(do_bandwidth)) {
        mib[0] = CTL_NET;
        mib[1] = PF_ROUTE;
        mib[2] = 0;
        mib[3] = AF_INET;
        mib[4] = NET_RT_IFLIST2;
        mib[5] = 0;
        if (unlikely(sysctl(mib, 6, NULL, &size, NULL, 0))) {
            error("MACOS: sysctl(%s...) failed: %s", "net interfaces", strerror(errno));
            do_bandwidth = 0;
            error("DISABLED: system.ipv4");
        } else {
            ifstatdata = reallocz(ifstatdata, size);
            if (unlikely(sysctl(mib, 6, ifstatdata, &size, NULL, 0) < 0)) {
                error("MACOS: sysctl(%s...) failed: %s", "net interfaces", strerror(errno));
                do_bandwidth = 0;
                error("DISABLED: system.ipv4");
            } else {
                lim = ifstatdata + size;
                iftot.ift_ibytes = iftot.ift_obytes = 0;
                for (next = ifstatdata; next < lim; ) {
                    ifm = (struct if_msghdr *)next;
                    next += ifm->ifm_msglen;

                    if (ifm->ifm_type == RTM_IFINFO2) {
                        struct if_msghdr2 *if2m = (struct if_msghdr2 *)ifm;

                        iftot.ift_ibytes += if2m->ifm_data.ifi_ibytes;
                        iftot.ift_obytes += if2m->ifm_data.ifi_obytes;
                    }
                }
                st = rrdset_find("system.ipv4");
                if (unlikely(!st)) {
                    st = rrdset_create("system", "ipv4", NULL, "network", NULL, "IPv4 Bandwidth", "kilobits/s", 500, update_every, RRDSET_TYPE_AREA);

                    rrddim_add(st, "InOctets", "received", 8, 1024, RRDDIM_INCREMENTAL);
                    rrddim_add(st, "OutOctets", "sent", -8, 1024, RRDDIM_INCREMENTAL);
                }
                else rrdset_next(st);

                rrddim_set(st, "InOctets", iftot.ift_ibytes);
                rrddim_set(st, "OutOctets", iftot.ift_obytes);
                rrdset_done(st);
            }
        }
    }

    return 0;
}

int getsysctl(const char *name, void *ptr, size_t len)
{
    size_t nlen = len;

    if (unlikely(sysctlbyname(name, ptr, &nlen, NULL, 0) == -1)) {
        error("MACOS: sysctl(%s...) failed: %s", name, strerror(errno));
        return 1;
    }
    if (unlikely(nlen != len)) {
        error("MACOS: sysctl(%s...) expected %lu, got %lu", name, (unsigned long)len, (unsigned long)nlen);
        return 1;
    }
    return 0;
}
