// RFC 0020 (since 0.18.0): production_model_rc.frag with the cover laid by facing and still ice
// (BTBH_COVER). A batch whose cover layer is on, or a still-ice batch, draws with it; every other batch
// with production_model_rc.frag, unchanged.
#define BTBH_COVER 1
#define BTBH_DIRECTIONAL_SHADOWS 1
#define BTBH_OMNI_SHADOWS 1
#define BTBH_RC_MATERIAL_OUTPUT 1
#include "production_model.frag.hlsl"
