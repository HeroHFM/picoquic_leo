// Author: Victor Kamel (vkamel@uvic.ca)

#include "sat_utils.h"

#include <sys/time.h>
#include <stdint.h>
#include <time.h>
#include <math.h>

static const uint64_t s_to_us = 1000000ull;

// TODO: Reconfigure to use a reference time and current_time
bool picoquic_check_handover()
{
	struct timeval tv;
    (void)gettimeofday(&tv, NULL);

    const uint64_t usecond = ((tv.tv_sec * s_to_us) + tv.tv_usec) % (60 * s_to_us);

    for (size_t i = 0; i < SL_HANDOVER_COUNT; ++i) {
    	if (abs((SL_HANDOVER_INTERVALS[i]  * s_to_us) - usecond) <= MARGIN) return true;
    }

    return false;

}