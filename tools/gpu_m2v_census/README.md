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

Zero constants are diagnostic inputs, not CoreAnimation's runtime values; pairings and vertex
layouts are probe inputs, not recovered CA pipelines. PASS means metal2vulkan translated and
verified the pipeline and `spirv-val` accepted it; it does not mean Turnip compiled it to A840 code
or that it renders correctly.
