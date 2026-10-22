# QuartzCore shader translation census (metal2vulkan)

Offline check that iOS 16.7.16's CoreAnimation shaders translate from Metal AIR to Vulkan SPIR-V
with metal2vulkan, for a device that has `VK_EXT_rasterization_order_attachment_access` (Turnip
exposes it on every Adreno) and for one without it. No phone, no GPU: translation and `spirv-val`
only. Results are in `docs/GPU_PLAN.md` ("着色器翻译实验").

Inputs (Apple files, never committed): `QuartzCore.framework/default.metallib` (SHA-256
`11c74cc4…`) and `default.pipelinelib/functions` from the iOS root image, copied to
`/opt/work/m2vtest/` in WSL.

1. `metal2vulkan` from steelbrain/metal2vulkan (a2c440b) copied to `/opt/work/m2v`, built with
   `cargo build --release -p metal2vulkan --features cli`. `patch_cli.py` adds a test-only
   `--fragment-library FILE` option to that private copy, so a fragment can come from another
   metallib (each pipeline-library record holds one function).
2. `mtlb_list.py FILE`: entry functions of a metallib (type and name).
3. `split_functions.py functions OUTDIR`: the pipeline library's one-function MTLB records.
4. `run.sh PROFILE`: every main-library entry point (fragments paired with `std_vert0_lpf`,
   vertices with `path_clear_frag_lpf`, BGRA8Unorm, eight Float4 attributes, stride 128 —
   `/opt/work/m2vtest/vd8.json`). `retry.sh` finds a matching vertex and extra attachments for the
   failures; `retry2.sh` gives `path_blit_*` three attachments (BGRA8, BGRA8, R32Float, as
   UniXNU's quartzcore_blur_host.mm) and fills the `fixed_*` uber shaders' constants with zeros.
5. `run_plib.sh PROFILE` / `retry_plib.sh`: the same for the 451 pipeline-library records,
   fragments paired with the specialized `fixed_vert_lph_gen` record.

6. `emit_all.sh OUTDIR` (profile from `M2V_PROFILE`, default lavapipe): translates everything
   once more with the first passing probe configuration, keeping the SPIR-V and an
   `--emit-meta` file per pipeline.
7. Turnip on an emulated Adreno 840: Mesa main built in WSL with
   `-Dvulkan-drivers=freedreno -Dfreedreno-kmds=msm -Dtools=drm-shim -Dgallium-drivers= -Dplatforms=`;
   `src/freedreno/drm-shim/freedreno_noop.c` got a local entry `{ .gpu_id = 840,
   .chip_id = 0x44050a31, .gmem_size = 18 * 1024 * 1024 }`. `/opt/work/m2vtest/tu_icd.json` is the
   build's ICD JSON with `library_path` pointing at the built `libvulkan_freedreno.so`.
   Turnip's real capabilities: `vulkaninfo --json` under the shim → `turnip_a840.json`; use it as
   `M2V_PROFILE` for step 6.
8. `run_tu.sh DIR [CHIP_ID]` (`MESA_BUILD` picks the Mesa build): one process per meta runs `vk_pipeline.py`, which creates the
   Vulkan pipeline (layouts, push constants, specialization, vertex input, dynamic rendering) and
   prints Turnip's per-stage instruction counts (VK_KHR_pipeline_executable_properties).

Zero constants are diagnostic inputs, not CoreAnimation's runtime values; pairings and vertex
layouts are probe inputs, not recovered CA pipelines. PASS means metal2vulkan translated and
verified the pipeline and `spirv-val` accepted it; it does not mean Turnip compiled it to A840 code
or that it renders correctly.
9. `run_kernels.sh PROFILE OUTDIR LIB...`: every compute kernel of the given metallibs (used for
   CoreImage's own kernels), then `run_tu.sh OUTDIR`.
