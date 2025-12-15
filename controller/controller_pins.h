/**
 * Z1 Onyx Controller - Hardware Pin Definitions
 * 
 * Hardware variant selector - includes appropriate pin configuration
 */

#ifndef CONTROLLER_PINS_H
#define CONTROLLER_PINS_H

#ifdef HW_V1
#include "controller_pins_v1.h"
#elif defined(HW_V2)
#include "controller_pins_v2.h"
#else
#error "Hardware version not defined! Define HW_V1 or HW_V2"
#endif// Common hardware configuration
#define MAX_NODES 16  // V2 hardware supports 16 nodes

#endif // CONTROLLER_PINS_H

