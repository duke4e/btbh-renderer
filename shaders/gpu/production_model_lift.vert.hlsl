// RFC 0012: the static vertex shader of a batch with a height lift (BTBHGPUSceneGeometryBatch.height_lift):
// production_model.vert with every vertex lifted by a heightfield (production_height_lift.hlsli). The core loads it
// beside the host's production_model.vert, and only for such batches.
#define BTBH_HEIGHT_LIFT 1
#include "production_model.vert.hlsl"
