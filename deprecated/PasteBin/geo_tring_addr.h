/*
 * geo_tring_addr.h — bridge to the active GeoPixel update lane
 */
#ifndef GEO_TRING_ADDR_H
#define GEO_TRING_ADDR_H

#if __has_include("../geopixel/geopixel/geo_tring_addr.h")
#include "../geopixel/geopixel/geo_tring_addr.h"
#else
#error "geo_tring_addr.h: no active tring address implementation found"
#endif

#endif /* GEO_TRING_ADDR_H */
