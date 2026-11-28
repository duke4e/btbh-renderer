// RFC 0020 (since 0.18.0): production_model_2d_shadows.frag with the cover laid by facing and still ice
// (BTBH_COVER). A batch whose cover layer is on, or a still-ice batch, draws with it; every other batch
// with production_model_2d_shadows.frag, unchanged.
#define BTBH_COVER 1
#define BTBH_OMNI_SHADOWS 0
#include "production_model.frag.hlsl"
