#!/usr/bin/env python3
"""Regenerate the renderer's FSR 3.1 upscaler shaders (shaders/gpu/fsr3, shaders/gpu/production_fsr3_*.comp.hlsl)
from a checkout of AMD's FidelityFX SDK (tag v2.0.0: FSR upscaler 3.1.5).

    python tools/fsr3/vendor_fsr3.py <FidelityFX-SDK checkout>

Upstream headers are copied byte for byte except three include paths; the SDL GPU binding layer and the
pass entry points are generated. See shaders/gpu/fsr3/README.md.
"""
import os
import re
import shutil
import sys

RENDERER = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
SHADERS = os.path.join(RENDERER, "shaders", "gpu")
DEST = os.path.join(SHADERS, "fsr3")

INCLUDE_PATCHES = {
    "ffx_fsr3upscaler_luma_pyramid.h": [('#include "spd/ffx_spd.h"', '#include "../spd/ffx_spd.h"')],
    "ffx_fsr3upscaler_shading_change_pyramid.h": [('#include "spd/ffx_spd.h"', '#include "../spd/ffx_spd.h"')],
    "ffx_fsr3upscaler_rcas.h": [('#include "../../../../../api/internal/gpu/ffx_core.h"', '#include "../ffx/ffx_core.h"')],
}

# Storage formats of the read-write textures, as btbh_gpu_fsr3.c creates them.
UAV_FORMATS = {
    "rw_luma_history": "rgba16f",
    "rw_internal_upscaled_color": "rgba16f",
    "rw_upscaled_output": "rgba16f",
    "rw_accumulation": "r8",
    "rw_shading_change": "r8",
    "rw_farthest_depth": "r16f",
    "rw_farthest_depth_mip1": "r16f",
    "rw_current_luma": "r16f",
    "rw_luma_instability": "r16f",
    "rw_new_locks": "r8",
    "rw_reconstructed_previous_nearest_depth": "r32ui",
    "rw_dilated_depth": "r32f",
    "rw_dilated_motion_vectors": "rg16f",
    "rw_dilated_reactive_masks": "rgba8",
    "rw_output_autoreactive": "r8",
    "rw_output_autocomposition": "r8",
    "rw_output_prev_color_pre_alpha": "r11g11b10f",
    "rw_output_prev_color_post_alpha": "r11g11b10f",
    "rw_frame_info": "rgba32f",
    "rw_spd_mip0": "rg16f",
    "rw_spd_mip1": "rg16f",
    "rw_spd_mip2": "rg16f",
    "rw_spd_mip3": "rg16f",
    "rw_spd_mip4": "rg16f",
    "rw_spd_mip5": "rg16f",
    "rw_spd_global_atomic": "r32ui",
}

# Pass entry points: read-only and read-write textures in the order btbh_gpu_fsr3.c binds them.
PASSES = [
    ("prepare_inputs",
     "dilates depth and motion vectors, reconstructs the previous frame's nearest depth and stores this frame's luma",
     ["INPUT_MOTION_VECTORS", "INPUT_DEPTH", "INPUT_COLOR"],
     ["DILATED_MOTION_VECTORS", "DILATED_DEPTH", "RECONSTRUCTED_PREV_NEAREST_DEPTH", "FARTHEST_DEPTH", "CURRENT_LUMA"],
     ["FSR3UPSCALER"], ["ffx_fsr3upscaler_prepare_inputs.h"], (8, 8),
     "void main(int2 iDispatchThreadId : SV_DispatchThreadID)\n{\n    PrepareInputs(iDispatchThreadId);\n}"),
    ("luma_pyramid",
     "reduces luma and farthest depth (single-pass downsampler) and updates the frame info (exposure, average luma)",
     ["CURRENT_LUMA", "FARTHEST_DEPTH"],
     # Only mip 5 of the pyramid (see write_callbacks).
     ["SPD_GLOBAL_ATOMIC", "FRAME_INFO", "SPD_MIPS_LEVEL_5", "FARTHEST_DEPTH_MIP1"],
     ["FSR3UPSCALER", "SPD"], ["ffx_fsr3upscaler_luma_pyramid.h"], (256, 1),
     "void main(uint3 WorkGroupId : SV_GroupID, uint LocalThreadIndex : SV_GroupIndex)\n{\n    ComputeAutoExposure(WorkGroupId, LocalThreadIndex);\n}"),
    ("shading_change_pyramid",
     "builds the pyramid of luma differences between this frame and the reprojected previous one",
     ["CURRENT_LUMA", "PREVIOUS_LUMA", "DILATED_MOTION_VECTORS", "INPUT_EXPOSURE"],
     ["SPD_GLOBAL_ATOMIC", "SPD_MIPS_LEVEL_0", "SPD_MIPS_LEVEL_1", "SPD_MIPS_LEVEL_2", "SPD_MIPS_LEVEL_3",
      "SPD_MIPS_LEVEL_4", "SPD_MIPS_LEVEL_5"],
     ["FSR3UPSCALER", "SPD"], ["ffx_fsr3upscaler_shading_change_pyramid.h"], (256, 1),
     "void main(uint3 WorkGroupId : SV_GroupID, uint LocalThreadIndex : SV_GroupIndex)\n{\n    ComputeShadingChangePyramid(WorkGroupId, LocalThreadIndex);\n}"),
    ("shading_change", "turns the shading-change pyramid into one half-resolution change mask",
     ["SPD_MIPS"], ["SHADING_CHANGE"], ["FSR3UPSCALER"], ["ffx_fsr3upscaler_shading_change.h"], (8, 8),
     "void main(int2 iDispatchThreadId : SV_DispatchThreadID)\n{\n    ShadingChange(iDispatchThreadId);\n}"),
    ("prepare_reactivity",
     "combines the reactive mask, disocclusion and shading change, and updates the accumulation and new locks",
     ["RECONSTRUCTED_PREV_NEAREST_DEPTH", "DILATED_MOTION_VECTORS", "DILATED_DEPTH", "REACTIVE_MASK",
      "TRANSPARENCY_AND_COMPOSITION_MASK", "ACCUMULATION", "SHADING_CHANGE", "CURRENT_LUMA", "INPUT_EXPOSURE"],
     ["DILATED_REACTIVE_MASKS", "NEW_LOCKS", "ACCUMULATION"],
     ["FSR3UPSCALER"], ["ffx_fsr3upscaler_prepare_reactivity.h"], (8, 8),
     "void main(int2 iDispatchThreadId : SV_DispatchThreadID)\n{\n    PrepareReactivity(iDispatchThreadId);\n}"),
    ("luma_instability", "measures how much each pixel's luma flickers across the last frames",
     ["INPUT_EXPOSURE", "DILATED_REACTIVE_MASKS", "DILATED_MOTION_VECTORS", "FRAME_INFO", "LUMA_HISTORY",
      "FARTHEST_DEPTH_MIP1", "CURRENT_LUMA"],
     ["LUMA_HISTORY", "LUMA_INSTABILITY"],
     ["FSR3UPSCALER"], ["ffx_fsr3upscaler_luma_instability.h"], (8, 8),
     "void main(int2 iDispatchThreadId : SV_DispatchThreadID)\n{\n    LumaInstability(iDispatchThreadId);\n}"),
    ("accumulate", "upsamples this frame and blends it into the reprojected history at the output resolution",
     ["INPUT_EXPOSURE", "DILATED_REACTIVE_MASKS", "DILATED_MOTION_VECTORS", "INTERNAL_UPSCALED",
      "FARTHEST_DEPTH_MIP1", "CURRENT_LUMA", "LUMA_INSTABILITY", "INPUT_COLOR"],
     ["INTERNAL_UPSCALED", "UPSCALED_OUTPUT", "NEW_LOCKS"],
     ["FSR3UPSCALER"],
     ["ffx_fsr3upscaler_sample.h", "ffx_fsr3upscaler_upsample.h", "ffx_fsr3upscaler_reproject.h",
      "ffx_fsr3upscaler_accumulate.h"], (8, 8),
     "void main(int2 iDispatchThreadId : SV_DispatchThreadID)\n{\n    Accumulate(iDispatchThreadId);\n}"),
    ("debug_view", "draws AMD's debug view (dilated motion, depth, reactive masks, history) over the output",
     ["DILATED_REACTIVE_MASKS", "DILATED_MOTION_VECTORS", "DILATED_DEPTH", "INTERNAL_UPSCALED", "INPUT_EXPOSURE"],
     ["UPSCALED_OUTPUT"], ["FSR3UPSCALER"], ["ffx_fsr3upscaler_debug_view.h"], (8, 8),
     "void main(int2 iPxPos : SV_DispatchThreadID)\n{\n    DebugView(iPxPos);\n}"),
]


def copy_upstream(sdk):
    core = os.path.join(sdk, "Kits", "FidelityFX", "api", "internal", "gpu")
    upscaler = os.path.join(sdk, "Kits", "FidelityFX", "upscalers", "fsr3", "include", "gpu")
    copies = [(os.path.join(core, name), os.path.join(DEST, "ffx", name)) for name in [
        "ffx_common_types.h", "ffx_core.h", "ffx_core_gpu_common.h", "ffx_core_gpu_common_half.h",
        "ffx_core_hlsl.h", "ffx_core_portability.h"]]
    copies.append((os.path.join(upscaler, "spd", "ffx_spd.h"), os.path.join(DEST, "spd", "ffx_spd.h")))
    for name in sorted(os.listdir(os.path.join(upscaler, "fsr3upscaler"))):
        if name != "ffx_fsr3upscaler_callbacks_hlsl.h":
            copies.append((os.path.join(upscaler, "fsr3upscaler", name), os.path.join(DEST, "fsr3upscaler", name)))
    for source, destination in copies:
        os.makedirs(os.path.dirname(destination), exist_ok=True)
        data = open(source, "rb").read()
        for old, new in INCLUDE_PATCHES.get(os.path.basename(source), []):
            if data.count(old.encode()) != 1:
                sys.exit(f"include to patch not found once in {source}: {old}")
            data = data.replace(old.encode(), new.encode())
        open(destination, "wb").write(data)
    shutil.copyfile(os.path.join(sdk, "docs", "license.md"), os.path.join(DEST, "LICENSE.md"))
    return os.path.join(upscaler, "fsr3upscaler", "ffx_fsr3upscaler_callbacks_hlsl.h")


def write_callbacks(source):
    text = open(source, "r", newline="").read()
    licence_end = text.index("// THE SOFTWARE.") + len("// THE SOFTWARE.")
    licence, body = text[:licence_end], text[licence_end:]
    for old, new in [
        ('#include "ffx_fsr3upscaler_resources.h"', '#include "fsr3upscaler/ffx_fsr3upscaler_resources.h"'),
        ('#include "../../../../../api/internal/gpu/ffx_core.h"', '#include "ffx/ffx_core.h"'),
        ("SamplerState s_PointClamp FFX_DECLARE_SAMPLER(0);\n", ""),
        ("SamplerState s_LinearClamp FFX_DECLARE_SAMPLER(1);\n", ""),
    ]:
        if body.count(old) != 1:
            sys.exit(f"expected one occurrence of {old!r} in the callbacks")
        body = body.replace(old, new)
    srv_names = []

    def declare_srv(match):
        prefix, name, binding = match.group(1), match.group(2), match.group(3)
        srv_names.append(name)
        return (f"{prefix}BTBH_FSR3_DECLARE_SRV({binding});\n"
                f"SamplerState {name}_sampler BTBH_FSR3_DECLARE_SAMPLER({binding});")

    body = re.sub(r"^(.*Texture2D<[^>]+>\s+(\w+)\s+)FFX_DECLARE_SRV\((\w+)\);\s*$", declare_srv, body, flags=re.MULTILINE)
    for name in srv_names:
        body = body.replace(f"{name}.SampleLevel(s_LinearClamp,", f"{name}.SampleLevel({name}_sampler,")
    if "s_LinearClamp" in body or "s_PointClamp" in body:
        sys.exit("a SampleLevel call still names a shared sampler")

    def declare_uav(match):
        coherent, declaration, name, binding = match.group(1) or "", match.group(2), match.group(3), match.group(4)
        if name not in UAV_FORMATS:
            sys.exit(f"no storage format for {name}")
        return f'[[vk::image_format("{UAV_FORMATS[name]}")]] {coherent}{declaration}BTBH_FSR3_DECLARE_UAV({binding});'

    body = re.sub(r"^(globallycoherent\s+)?(RWTexture2D<[^>]+>\s+(\w+)\s+)FFX_DECLARE_UAV\((\w+)\);\s*$",
                  declare_uav, body, flags=re.MULTILINE)
    body = body.replace("FFX_DECLARE_CB(", "BTBH_FSR3_DECLARE_CB(")
    # The luma pyramid stores and reloads only mip 5 of the downsampler's pyramid; declaring all six
    # mips would take it past the 8 read-write textures SDL GPU binds per compute pass.
    spd_end = """    STORE(5);

#undef STORE
}
#endif
"""
    if body.count(spd_end) != 1:
        sys.exit("the downsampler's mip block changed shape")
    body = body.replace(spd_end, spd_end[:-len("#endif\n")] + """#elif defined(FSR3UPSCALER_BIND_UAV_SPD_MIPS_LEVEL_5)

[[vk::image_format("rg16f")]] globallycoherent RWTexture2D<FfxFloat32x2>  rw_spd_mip5   BTBH_FSR3_DECLARE_UAV(FSR3UPSCALER_BIND_UAV_SPD_MIPS_LEVEL_5);

FfxFloat32x2 RWLoadPyramid(FFX_PARAMETER_IN FfxInt32x2 iPxPos, FFX_PARAMETER_IN FfxUInt32 index)
{
    return rw_spd_mip5[iPxPos];
}

void StorePyramid(FFX_PARAMETER_IN FfxInt32x2 iPxPos, FFX_PARAMETER_IN FfxFloat32x2 outValue, FFX_PARAMETER_IN FfxUInt32 index)
{
    if (index == 5)
    {
        rw_spd_mip5[iPxPos] = outValue;
    }
}
#endif
""")
    header = licence + """
//
// Be The Bullet Hell: the SDL GPU binding layer for the FSR 3.1 upscaler, generated from AMD's
// ffx_fsr3upscaler_callbacks_hlsl.h (FidelityFX SDK v2.0.0, FSR 3.1.5) by tools/fsr3/vendor_fsr3.py. SDL GPU
// compute shaders take read-only textures as t<n> in space0, each paired with the sampler s<n> (the passes'
// two shared samplers became one per texture), read-write textures as u<n> in space1 with their storage
// format, and uniform buffers as b<n> in space2.

#define BTBH_FSR3_T(index) t##index
#define BTBH_FSR3_S(index) s##index
#define BTBH_FSR3_U(index) u##index
#define BTBH_FSR3_B(index) b##index
#define BTBH_FSR3_DECLARE_SRV(index) : register(BTBH_FSR3_T(index), space0)
#define BTBH_FSR3_DECLARE_SAMPLER(index) : register(BTBH_FSR3_S(index), space0)
#define BTBH_FSR3_DECLARE_UAV(index) : register(BTBH_FSR3_U(index), space1)
#define BTBH_FSR3_DECLARE_CB(index) : register(BTBH_FSR3_B(index), space2)
"""
    open(os.path.join(DEST, "btbh_fsr3_callbacks_hlsl.h"), "w", newline="\n").write(header + body)


def write_passes():
    for name, summary, srvs, uavs, cbs, headers, (width, height), entry in PASSES:
        lines = [
            f"// AMD FidelityFX Super Resolution 3.1 upscaler, {name.replace('_', ' ')} pass: {summary}.",
            f"// AMD's ffx_fsr3upscaler_{name}_pass.hlsl (FidelityFX SDK v2.0.0, FSR 3.1.5, MIT licence,",
            "// Copyright (c) 2025 Advanced Micro Devices, Inc.; see fsr3/LICENSE.md) on SDL GPU's compute bindings.",
            '#include "fsr3/btbh_fsr3_options.hlsli"',
            "",
        ]
        lines += [f"#define FSR3UPSCALER_BIND_SRV_{resource} {index}" for index, resource in enumerate(srvs)]
        lines += [f"#define FSR3UPSCALER_BIND_UAV_{resource} {index}" for index, resource in enumerate(uavs)]
        lines += [f"#define FSR3UPSCALER_BIND_CB_{resource} {index}" for index, resource in enumerate(cbs)]
        lines += ["", '#include "fsr3/btbh_fsr3_callbacks_hlsl.h"', '#include "fsr3/fsr3upscaler/ffx_fsr3upscaler_common.h"']
        lines += [f'#include "fsr3/fsr3upscaler/{header}"' for header in headers]
        lines += ["", f"[numthreads({width}, {height}, 1)]", entry, ""]
        path = os.path.join(SHADERS, f"production_fsr3_{name}.comp.hlsl")
        open(path, "w", newline="\n").write("\n".join(lines))


def main():
    if len(sys.argv) != 2:
        sys.exit(__doc__)
    write_callbacks(copy_upstream(sys.argv[1]))
    write_passes()
    print("FSR 3 shaders regenerated in", DEST)


if __name__ == "__main__":
    main()
