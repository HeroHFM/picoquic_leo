// Author: Victor Kamel (vkamel@uvic.ca)

#ifndef SAT_UTILS_H
#define SAT_UTILS_H

#include <stdbool.h>

#define SL_HANDOVER_INTERVALS ((const int[]) {12, 27, 42, 57})
#define SL_HANDOVER_COUNT 4

bool picoquic_check_handover();

#endif //SAT_UTILS_H