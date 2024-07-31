// Author: Victor Kamel (vkamel@uvic.ca)

#include "sat_utils.h"

#include <sys/time.h>
#include <time.h>
#include <math.h>

static inline int64_t ms_to_us(uint64_t s) { return s * 1000ull;    }
static inline int64_t  s_to_us(uint64_t s) { return s * 1000000ull; }

bool picoquic_check_handover(uint64_t ts)
{
    const int64_t usecond = ts % s_to_us(60);

    for (size_t i = 0; i < SL_HANDOVER_COUNT; ++i) {
        if (abs(s_to_us(SL_HANDOVER_INTERVALS[i]) - usecond) <= ms_to_us(MARGIN)) return true;
    }

    return false;
}


// TODO: Reconfigure to use a reference time and current_time
bool picoquic_check_handover_now()
{
	struct timeval tv;
    (void)gettimeofday(&tv, NULL);

    return picoquic_check_handover(s_to_us(tv.tv_sec) + tv.tv_usec);
}