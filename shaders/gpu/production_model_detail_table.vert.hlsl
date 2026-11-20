// RFC 0009: the static vertex shader of a batch with a world-detail table (BTBHGPUSceneGeometryBatch.world_detail_table):
// production_model.vert with the joint slot's first float read as location 5, the triangle's table index. The core
// loads it beside the host's production_model.vert, and only for such batches.
#define BTBH_WORLD_DETAIL_TABLE 1
#include "production_model.vert.hlsl"
