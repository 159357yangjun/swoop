#include "sched.h"

sched_action sched_decide(int enabled, int start_hhmm, int stop_hhmm, int cur_hhmm)
{
    if (!enabled) return SCHED_NONE;
    if (start_hhmm < 0 || start_hhmm > 2359) return SCHED_NONE;
    if (stop_hhmm  < 0 || stop_hhmm  > 2359) return SCHED_NONE;
    if (cur_hhmm   < 0 || cur_hhmm   > 2359) return SCHED_NONE;
    if (start_hhmm == stop_hhmm) return SCHED_NONE;   /* 起止相同 = 无意义 */
    if (cur_hhmm == start_hhmm) return SCHED_START;
    if (cur_hhmm == stop_hhmm)  return SCHED_STOP;
    return SCHED_NONE;
}
