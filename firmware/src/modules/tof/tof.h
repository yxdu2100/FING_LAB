#ifndef TOF_H
#define TOF_H

#include <stdint.h>

int tof_init(void);
int tof_measure_mm(uint16_t *distance_mm);

#endif
