// RFC 0018 (screen-space distortion, since 0.18.0): production_hdr_composite_distort.frag under FSR 3, also writing the
// reactive mask (a second target, blended with max) where the field displaces the scene.
#define BTBH_DISTORTION 2
#include "production_hdr_composite.frag.hlsl"
