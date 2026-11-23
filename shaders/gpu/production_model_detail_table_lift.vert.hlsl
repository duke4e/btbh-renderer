// RFC 0012 with RFC 0009: the static vertex shader of a batch with both a height lift and a world-detail table:
// production_model.vert with the table's index read (production_model_detail_table.vert) and every vertex lifted
// (production_model_lift.vert). The core loads it beside the host's production_model.vert, and only for such batches.
#define BTBH_WORLD_DETAIL_TABLE 1
#define BTBH_HEIGHT_LIFT 1
#include "production_model.vert.hlsl"
