// Author: Victor Kamel (vkamel@uvic.ca)

#include "sat_utils.h"

#include <sys/time.h>
#include <time.h>
#include <math.h>

// TODO: Reconfigure to use a reference time and current_time
bool picoquic_check_handover()
{
	struct timeval tv;
    (void)gettimeofday(&tv, NULL);

    const unsigned int margin = 1;
    const int second = tv.tv_sec % 60;

    for (size_t i = 0; i < SL_HANDOVER_COUNT; ++i) {
    	if (abs(SL_HANDOVER_INTERVALS[i] - second) <= margin) return true;
    }

    return false;

    // switch (tv.tv_sec % 60)
    // {
    // 	case 12:
    // 	case 27:
    // 	case 42:
    // 	case 57:
    // 		return true;
    // 		break;
    // }

    // return false;
}