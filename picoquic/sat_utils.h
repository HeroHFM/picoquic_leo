// Author: Victor Kamel (vkamel@uvic.ca)

#ifndef SAT_UTILS_H
#define SAT_UTILS_H

#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>

#define SL_HANDOVER_INTERVALS ((const int[]) {12, 27, 42, 57})
#define SL_HANDOVER_COUNT 4
#define MARGIN 100 /* ms */

bool picoquic_check_handover(uint64_t);
bool picoquic_check_handover_now();

#endif //SAT_UTILS_H