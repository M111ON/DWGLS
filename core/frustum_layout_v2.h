/*
 * frustum_layout_v2.h — bridge to the active updates lane
 */
#ifndef FRUSTUM_LAYOUT_V2_H
#define FRUSTUM_LAYOUT_V2_H

#if __has_include("../collection/active_updates/frustum_layout_v2.h")
#include "../collection/active_updates/frustum_layout_v2.h"
#else
#error "frustum_layout_v2.h: no active handoff implementation found"
#endif

#endif /* FRUSTUM_LAYOUT_V2_H */
