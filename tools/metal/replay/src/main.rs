//! m2v-replay: replay one Metal draw through metal2vulkan on whatever Vulkan device the loader
//! gives (lavapipe on the desktop), from a line-based spec file. See `USAGE`.

mod image;
mod spec;

use metal2vulkan::bind::SamplerDescriptor;
use metal2vulkan::cache::PipelineArtifact;
use metal2vulkan::library::{FunctionConstantValue, FunctionConstantValues, FunctionStage};
use metal2vulkan::metal::{
    BlendFactor, BlendOperation, ColorWriteMask, DataType, IndexType, PixelFormat, PrimitiveType, SamplerAddressMode,
    SamplerBorderColor, SamplerMinMagFilter, SamplerMipFilter, TextureUsage, VertexFormat, VertexStepFunction,
};
use metal2vulkan::pipeline::{
    ColorAttachmentDescriptor, RenderPipeline, RenderPipelineDescriptor, StageSource, VertexAttributeDescriptor,
    VertexBufferLayoutDescriptor, VertexDescriptor,
};
use metal2vulkan::Budget;
use metal2vulkan_runtime::{
    BlitOption, ClearColor, CommandBufferStatus, Device, DeviceOptions, LoadAction, MessageSink, MetalLevel, Origin,
    PipelineOptions, Region, RenderPassDescriptor, ResourceOptions, ScissorRect, Size, StoreAction, Texture,
    TextureDescriptor, Viewport,
};
use spec::Spec;
use std::fmt::Write as _;
use std::path::{Path, PathBuf};
use std::process::Command;
use std::sync::Mutex;
use std::time::Duration;

const USAGE: &str = "usage: m2v-replay <spec.txt> [out-dir]   (out-dir defaults to <spec>.out)

Spec: one directive per line, '#' comments.
  library <path>                      .metallib (fat ok: the AIR64 slice), .air bitcode, or .ll text (llvm-as)
  vertex <name> [const...]            constants: name:type:hex or #index:type:hex (type = MTLDataType)
  fragment <name> [const...]
  vconst <const...> / fconst <const...>   more constants for the stage
  attachment <i> format=80 size=WxH load=clear|load|dontcare clear=r,g,b,a [fill=r,g,b,a]
             [blend=on|off] [rgb=src,dst,op] [alpha=src,dst,op] [mask=15] [store=store|dontcare] [pipeline=1|0] [expect=r,g,b,a~tol]
                                      pipeline=0: in the pass only, not in the pipeline descriptor
                                      fill: the attachment's contents before the pass (a clear pass)
                                      expect: 0..255 per channel (as stored), every 8x8 cell within tol
  attr aN=format:offset:buffer:stride[:step:rate] ...   MTLVertexFormat numbers
  buffer v|f <index> [len=N] [offset=N] [data=N] [hex=..] [f32=a,b,..] [file=path]   (zero padded to len)
                                      bound at offset; the bytes go at data (default: offset)
  patch v|f <index> at=<buffer offset> hex=..|f32=..   overwrite bytes of an earlier buffer line
  late-patch v|f <index> at=.. hex=..|f32=..   the same, written into the buffer after encoding, before commit
  bytes v|f <index> <hex> | f32=a,b,..       setVertexBytes / setFragmentBytes
  sampler v|f <index> [filter=|min=|mag=nearest|linear] [mip=none|nearest|linear]
             [address=|s=|t=|r=clamp|mirror_clamp|repeat|mirror|zero|border] [norm=1] [lodmin=] [lodmax=]
  texture v|f <index> format=N size=WxH fill=solid r g b a | fill=astc-void r g b a | fill=file path
             | fill=astc-void-split <blockcols> r g b a r g b a   (left block columns, then the rest)
  draw <triangle|trianglestrip|line|point|N> count=N [start=0] [instances=1]
  drawindexed <prim> count=N type=16|32 indices=<hex> [offset=0] [instances=1] [basevertex=0]
  viewport x y w h [znear zfar] / scissor x y w h
  compile-only                         no draw, SPIR-V only

Several draws in one pass: `begin [label]` (or `== draw N`) starts each draw's lines; the lines
before the first one belong to draw 0. Pass-level lines anywhere: library, attachment, rt, grid,
prefixes, policy, astc, compile-only, list-functions, autofill-missing, keep-unknown. Per draw:
vertex/fragment/vconst/fconst/attr/color (a new pipeline; a draw without them keeps the last),
buffer/patch/late-patch/bytes/sampler/texture/viewport/scissor (set on the encoder, and kept for
later draws as Metal keeps them), draw/drawindexed.
  color <i> format=80 [mask=15] [rgb=..] [alpha=..]   the draw's pipeline colour attachment
                                       (without color lines the pass's attachments are used)
  rt WxH                               render target size (render_target_width/height)
  grid x y w h                         the region the 8x8 grid averages (default: the render target)
  prefixes                             also replay draws 0..k for every k and print each grid
  list-functions                       print the library's functions and their constants
  autofill-missing                     a constant the compile insists on gets zero bytes (reported)
  keep-unknown                         pass constants the function does not declare (default: dropped)
  policy default|exact                 DevicePolicy::UNTRUSTED (default) or .exact_answers()
  astc auto|native|decode              auto: native ASTC, a CPU-decoded RGBA8 stand-in where the device has none
";

static MESSAGES: Mutex<Vec<String>> = Mutex::new(Vec::new());

fn note(line: String) {
    println!("{line}");
    MESSAGES.lock().unwrap().push(line);
}

type R<T> = Result<T, String>;

fn e<E: std::fmt::Debug>(what: &'static str) -> impl FnOnce(E) -> String {
    move |error| format!("{what}: {error:?}")
}

fn raw<T: TryFrom<u32>>(value: u32, what: &str) -> R<T> {
    T::try_from(value).map_err(|_| format!("{what} {value} is not a Metal value"))
}

fn main() {
    let args: Vec<String> = std::env::args().collect();
    if args.len() < 2 || args[1] == "-h" || args[1] == "--help" {
        eprint!("{USAGE}");
        std::process::exit(2);
    }
    let spec_path = PathBuf::from(&args[1]);
    let out = args.get(2).map(PathBuf::from).unwrap_or_else(|| spec_path.with_extension("out"));
    match run(&spec_path, &out) {
        Ok(code) => std::process::exit(code),
        Err(why) => {
            note(format!("FAIL: {why}"));
            let _ = std::fs::write(out.join("messages.txt"), MESSAGES.lock().unwrap().join("\n") + "\n");
            std::process::exit(1);
        }
    }
}

/// The AIR slice of a universal metallib, as Metal picks it (NativeMetalBridge.rs `air_slice`).
fn air_slice(bytes: &[u8]) -> Option<&[u8]> {
    const AIR64: u32 = 0x0100_0017;
    let word = |at: usize| bytes.get(at..at + 4).map(|w| u32::from_be_bytes([w[0], w[1], w[2], w[3]]));
    if word(0)? != 0xcafe_babe {
        return Some(bytes);
    }
    let count = word(4)? as usize;
    for i in 0..count.min(16) {
        let at = 8 + 20 * i;
        if word(at)? == AIR64 {
            let (offset, size) = (word(at + 8)? as usize, word(at + 12)? as usize);
            return bytes.get(offset..offset.checked_add(size)?);
        }
    }
    None
}

fn llvm_as() -> PathBuf {
    if let Some(p) = std::env::var_os("LLVM_AS") {
        return PathBuf::from(p);
    }
    let sysroot = Command::new("rustup")
        .args(["run", "1.98.0", "rustc", "--print", "sysroot"])
        .output()
        .ok()
        .map(|o| String::from_utf8_lossy(&o.stdout).trim().to_string());
    if let Some(s) = sysroot {
        let p = PathBuf::from(s).join("lib/rustlib/x86_64-unknown-linux-gnu/bin/llvm-as");
        if p.exists() {
            return p;
        }
    }
    PathBuf::from("llvm-as")
}

fn compile_library(path: &Path) -> R<metal2vulkan::library::Library> {
    let bytes = std::fs::read(path).map_err(|e| format!("read {}: {e}", path.display()))?;
    let ext = path.extension().and_then(|x| x.to_str()).unwrap_or("");
    match ext {
        "ll" => {
            let out = Command::new(llvm_as())
                .arg("-o")
                .arg("-")
                .arg(path)
                .output()
                .map_err(|e| format!("llvm-as: {e}"))?;
            if !out.status.success() {
                return Err(format!("llvm-as: {}", String::from_utf8_lossy(&out.stderr)));
            }
            metal2vulkan::library::Library::from_air(&out.stdout).map_err(e("AIR library"))
        }
        "air" | "bc" => metal2vulkan::library::Library::from_air(&bytes).map_err(e("AIR library")),
        _ => {
            let slice = air_slice(&bytes).ok_or("metallib without an AIR64 slice")?;
            metal2vulkan::library::Library::from_metallib(slice).map_err(e("metallib"))
        }
    }
}

/// The byte size of a scalar or vector MTLDataType, where known; 3-vectors may also come padded.
fn data_type_sizes(ty: u32) -> Option<Vec<usize>> {
    let (elem, base) = match ty {
        3..=6 => (4, 3),
        16..=19 => (2, 16),
        29..=32 => (4, 29),
        33..=36 => (4, 33),
        37..=40 => (2, 37),
        41..=44 => (2, 41),
        45..=48 => (1, 45),
        49..=52 => (1, 49),
        53..=56 => (1, 53),
        81..=84 => (8, 81),
        85..=88 => (8, 85),
        _ => return None,
    };
    let n = (ty - base + 1) as usize;
    Some(if n == 3 { vec![3 * elem, 4 * elem] } else { vec![n * elem] })
}

fn data_type_name(ty: Option<DataType>) -> String {
    ty.map(|t| format!("{t:?}({})", t.as_raw())).unwrap_or_else(|| "?".into())
}

/// The function constant values of `stage` checked against what the function declares.
fn constants_for(
    lib: &metal2vulkan::library::Library,
    stage: &spec::Stage,
    drop_unknown: bool,
    log: &mut String,
) -> R<(FunctionConstantValues, Vec<metal2vulkan::library::FunctionConstant>)> {
    let info = lib
        .functions()
        .find(|f| f.name == stage.name)
        .ok_or(format!("the library has no function {:?}", stage.name))?;
    let declared: Vec<_> = info.constants.to_vec();
    let mut values = FunctionConstantValues::defaults();
    for b in &stage.broken {
        let _ = writeln!(log, "  BROKEN (skipped): {b}");
    }
    let mut given = std::collections::BTreeSet::new();
    for c in &stage.constants {
        let found = declared.iter().find(|d| match (&c.name, c.index) {
            (Some(n), _) => &d.name == n,
            (None, Some(i)) => d.index == Some(i),
            _ => false,
        });
        let Some(d) = found else {
            if drop_unknown {
                let _ = writeln!(log, "  UNKNOWN (not declared by {}, skipped): {}", stage.name, c.text);
                continue;
            }
            let _ = writeln!(log, "  UNKNOWN (not declared by {}, passed anyway): {}", stage.name, c.text);
            let value = FunctionConstantValue::new(raw::<DataType>(c.ty, "constant type")?, c.bytes.clone());
            match (&c.name, c.index) {
                (Some(n), _) => values.by_name.insert(n.clone(), value),
                (None, Some(i)) => values.by_index.insert(i, value),
                _ => None,
            };
            continue;
        };
        if let Some(sizes) = data_type_sizes(c.ty) {
            if !sizes.contains(&c.bytes.len()) {
                let _ = writeln!(log, "  BROKEN (type {} wants {:?} bytes, got {}; skipped): {}", c.ty, sizes, c.bytes.len(), c.text);
                continue;
            }
        }
        if let Some(t) = d.ty {
            if t.as_raw() != c.ty {
                let _ = writeln!(log, "  TYPE MISMATCH (declared {}, given {}; passed as given): {}", data_type_name(d.ty), c.ty, c.text);
            }
        }
        let Ok(ty) = raw::<DataType>(c.ty, "constant type") else {
            let _ = writeln!(log, "  BROKEN (type {} unknown; skipped): {}", c.ty, c.text);
            continue;
        };
        let value = FunctionConstantValue::new(ty, c.bytes.clone());
        given.insert(d.name.clone());
        match (&c.name, c.index) {
            (Some(n), _) => values.by_name.insert(n.clone(), value),
            (None, Some(i)) => values.by_index.insert(i, value),
            _ => None,
        };
    }
    let missing: Vec<String> = declared
        .iter()
        .filter(|d| !given.contains(&d.name))
        .map(|d| format!("{}{}:{}{}", d.name, d.index.map(|i| format!("#{i}")).unwrap_or_default(), data_type_name(d.ty),
                         if d.raw_flag == Some(1) { " (required)" } else { "" }))
        .collect();
    let _ = writeln!(log, "  {} declares {} constants; {} given and used; not given: {}", stage.name, declared.len(), given.len(),
                     if missing.is_empty() { "none".to_string() } else { missing.join(" ") });
    Ok((values, declared))
}

fn device(spec: &Spec, cache: &Path) -> R<Device> {
    let mut options = DeviceOptions::default();
    options.metal_level = Some(MetalLevel::NEWEST);
    options.shader_cache = metal2vulkan_runtime::ShaderCacheOptions::in_directory(cache.to_path_buf());
    options.messages = Some(MessageSink::new(|m| {
        note(format!("[m2v {:?} {:?}] {} {}", m.severity, m.kind, m.id_name, m.text));
    }));
    match spec.policy.as_str() {
        "default" => {}
        "exact" => options.policy = options.policy.exact_answers(),
        other => return Err(format!("policy {other:?}: default or exact")),
    }
    Device::system_default_with(&options).map_err(e("device"))
}

fn run_tool(tool: &str, args: &[&str], input: Option<&Path>) -> (bool, String) {
    let mut c = Command::new(tool);
    c.args(args);
    if let Some(p) = input {
        c.arg(p);
    }
    match c.output() {
        Ok(o) => (o.status.success(), format!("{}{}", String::from_utf8_lossy(&o.stdout), String::from_utf8_lossy(&o.stderr))),
        Err(e) => (false, format!("{tool}: {e}")),
    }
}

fn stage_suffix(stage: metal2vulkan::vk::ShaderStage) -> String {
    format!("{stage:?}").to_lowercase()
}

/// Writes each stage's SPIR-V, its disassembly and spirv-val's verdict, and a meta file.
fn dump_pipeline(dev: &Device, p: &RenderPipeline, out: &Path, prefix: &str) -> R<()> {
    let plan = dev.compile_device().profile().spirv_validation();
    let mut meta = String::new();
    let _ = writeln!(meta, "key: {:?}\nvariant_bits: {:#x}", p.key, p.variant_bits);
    let _ = writeln!(meta, "\nlayout: {:#?}", p.layout);
    let _ = writeln!(meta, "\nvertex_input: {:#?}", p.vertex_input);
    let _ = writeln!(meta, "\ngraphics: {:#?}", p.graphics);
    let _ = writeln!(meta, "\ndependencies: {:#?}", p.dependencies);
    let _ = writeln!(meta, "\ninterface: {:#?}", p.interface);
    let _ = writeln!(meta, "\nrequirements: {:#?}", p.requirements);
    for s in &p.stages {
        let name = format!("{prefix}.{}", stage_suffix(s.stage));
        let spv = out.join(format!("{name}.spv"));
        let bytes: Vec<u8> = s.spirv.iter().flat_map(|w| w.to_le_bytes()).collect();
        std::fs::write(&spv, &bytes).map_err(|e| format!("write {}: {e}", spv.display()))?;
        let (ok, text) = run_tool("spirv-dis", &["--raw-id"], Some(&spv));
        std::fs::write(out.join(format!("{name}.spvasm")), if ok { text } else { format!("; spirv-dis failed\n{text}") })
            .map_err(|e| format!("write spvasm: {e}"))?;
        let mut verdicts = Vec::new();
        for env in &plan.target_envs {
            let mut a: Vec<&str> = vec!["--target-env", env];
            a.extend(plan.relaxations.iter().copied());
            let (ok, text) = run_tool("spirv-val", &a, Some(&spv));
            verdicts.push(if ok { format!("{env}: PASS") } else { format!("{env}: FAIL {}", text.trim()) });
        }
        note(format!("  {name}: {} words, entry {:?}; spirv-val {}", s.spirv.len(), s.entry_point, verdicts.join("; ")));
        if !s.precision_notes.is_empty() {
            note(format!("  {name}: precision notes: {:?}", s.precision_notes));
        }
        let _ = writeln!(meta, "\n== stage {name} entry {:?}\nspecialization: {:#?}\nrequired_subgroup_size: {:?}\nstage_flags: {:?}\nreconvergence: {:?}\nprecision_notes: {:#?}\nstrategies: {:#?}\nconstant_blob: {:?}",
            s.entry_point, s.specialization, s.required_subgroup_size, s.stage_flags, s.reconvergence, s.precision_notes, s.strategies,
            s.constant_blob.as_ref().map(|b| b.len()));
    }
    std::fs::write(out.join(format!("{prefix}.meta.txt")), meta).map_err(|e| format!("write meta: {e}"))?;
    Ok(())
}

fn render_descriptor(
    spec: &Spec,
    draw: &Spec,
    vertex: &metal2vulkan_runtime::Function,
    fragment: Option<&metal2vulkan_runtime::Function>,
) -> R<RenderPipelineDescriptor> {
    let mut d = RenderPipelineDescriptor::new(StageSource::Function(vertex.function_descriptor()));
    d.fragment = fragment.map(|f| StageSource::Function(f.function_descriptor()));
    let colors: Vec<(&u32, &spec::Attachment)> = if draw.colors.is_empty() {
        spec.attachments.iter().filter(|(_, a)| a.in_pipeline).collect()
    } else {
        draw.colors.iter().collect()
    };
    for (&i, a) in colors {
        let mut c = ColorAttachmentDescriptor::new(raw::<PixelFormat>(a.format, "color format")?);
        c.write_mask = raw::<ColorWriteMask>(a.write_mask, "write mask")?;
        c.blending_enabled = a.blend.enabled;
        c.source_rgb_blend_factor = raw::<BlendFactor>(a.blend.rgb.0, "blend factor")?;
        c.destination_rgb_blend_factor = raw::<BlendFactor>(a.blend.rgb.1, "blend factor")?;
        c.rgb_blend_operation = raw::<BlendOperation>(a.blend.rgb.2, "blend op")?;
        c.source_alpha_blend_factor = raw::<BlendFactor>(a.blend.alpha.0, "blend factor")?;
        c.destination_alpha_blend_factor = raw::<BlendFactor>(a.blend.alpha.1, "blend factor")?;
        c.alpha_blend_operation = raw::<BlendOperation>(a.blend.alpha.2, "blend op")?;
        *d.color_attachments.get_mut(i as usize).ok_or("attachment index past 7")? = Some(c);
    }
    if !draw.attributes.is_empty() {
        let mut inputs = VertexDescriptor::new();
        for (&i, a) in &draw.attributes {
            inputs.attributes.insert(i, VertexAttributeDescriptor::new(raw::<VertexFormat>(a.format, "vertex format")?, a.offset, a.buffer));
            let mut layout = VertexBufferLayoutDescriptor::new(Some(a.stride));
            layout.step_function = raw::<VertexStepFunction>(a.step_function, "step function")?;
            layout.step_rate = a.step_rate;
            inputs.layouts.insert(a.buffer, layout);
        }
        d.vertex_descriptor = Some(inputs);
    }
    Ok(d)
}

/// The runtime function `name` of `library` with `values`, retrying with zero bytes for each
/// constant the runtime or compile insists on when `autofill`.
fn function(
    library: &metal2vulkan_runtime::Library,
    name: &str,
    values: &FunctionConstantValues,
) -> R<metal2vulkan_runtime::Function> {
    library.new_function_with_name_constant_values(name, values).map_err(|err| format!("function {name}: {err}"))
}

fn is_astc(format: u32) -> bool {
    (186..=218).contains(&format) || (222..=235).contains(&format)
}

fn make_texture(dev: &Device, t: &spec::TextureSpec, astc: &str) -> R<(Texture, Option<String>)> {
    let native = || -> R<Texture> {
        let format = raw::<PixelFormat>(t.format, "texture format")?;
        let mut d = TextureDescriptor::texture_2d(format, t.width, t.height, false);
        d.usage = TextureUsage::SHADER_READ;
        d.resource_options = ResourceOptions::shared();
        let texture = dev.new_texture(&d).map_err(e("texture"))?;
        let (bytes, row) = image::texture_bytes(t.format, t.width, t.height, &t.fill)?;
        let rows = image::block_rows(t.format, t.height);
        texture
            .replace_region(Region::new_3d(0, 0, 0, t.width, t.height, 1), 0, 0, &bytes, row, row * rows)
            .map_err(e("replace region"))?;
        Ok(texture)
    };
    let decoded = || -> R<Texture> {
        // ASTC 4x4: decode to RGBA8 (sRGB stays sRGB).
        let srgb = image::astc_is_srgb(t.format);
        let rgba = image::astc_decode_fill(t.format, t.width, t.height, &t.fill)?;
        let format = if srgb { PixelFormat::RGBA8UnormSRGB } else { PixelFormat::RGBA8Unorm };
        let mut d = TextureDescriptor::texture_2d(format, t.width, t.height, false);
        d.usage = TextureUsage::SHADER_READ;
        d.resource_options = ResourceOptions::shared();
        let texture = dev.new_texture(&d).map_err(e("stand-in texture"))?;
        texture
            .replace_region(Region::new_3d(0, 0, 0, t.width, t.height, 1), 0, 0, &rgba, t.width * 4, t.width * 4 * t.height)
            .map_err(e("replace region"))?;
        Ok(texture)
    };
    if !is_astc(t.format) || astc == "native" {
        return native().map(|x| (x, None));
    }
    if astc == "decode" {
        return decoded().map(|x| (x, Some("ASTC decoded on the CPU to RGBA8 (astc decode)".into())));
    }
    match native() {
        Ok(x) => Ok((x, None)),
        Err(why) => decoded().map(|x| (x, Some(format!("native ASTC refused ({why}); CPU-decoded RGBA8 stand-in used")))),
    }
}

/// One draw's pipeline: specialize, dump the compile layer's SPIR-V under `prefix`, make the
/// runtime's state. Fills insisted-on constants with zeros when the spec asks.
fn make_pipeline(
    dev: &Device,
    lib: &metal2vulkan::library::Library,
    library: &metal2vulkan_runtime::Library,
    spec: &Spec,
    draw: &Spec,
    out: &Path,
    prefix: &str,
) -> R<metal2vulkan_runtime::RenderPipelineState> {
    let vstage = draw.vertex.clone().ok_or("a pipeline without a vertex line")?;
    let mut log = String::new();
    let (mut vvalues, vdecl) = constants_for(lib, &vstage, spec.drop_unknown, &mut log)?;
    let fspec = draw.fragment.clone();
    let mut fvalues_decl = None;
    if let Some(f) = &fspec {
        fvalues_decl = Some(constants_for(lib, f, spec.drop_unknown, &mut log)?);
    }
    for f in [&Some(vstage.clone()), &fspec].into_iter().flatten() {
        if let Some(info) = lib.functions().find(|i| i.name == f.name) {
            if matches!(info.stage, FunctionStage::Kernel) {
                return Err(format!("{} is a kernel", f.name));
            }
        }
    }
    print!("constants ({prefix}):\n{log}");
    MESSAGES.lock().unwrap().push(format!("constants ({prefix}):\n{log}"));
    let mut insisted: Vec<String> = Vec::new();
    let state = loop {
        let attempt = (|| -> R<metal2vulkan_runtime::RenderPipelineState> {
            let v = function(library, &vstage.name, &vvalues)?;
            let f = match (&fspec, &fvalues_decl) {
                (Some(f), Some((values, _))) => Some(function(library, &f.name, values)?),
                _ => None,
            };
            let d = render_descriptor(spec, draw, &v, f.as_ref())?;
            // The pipeline as the compile layer makes it with no draw-time variant: dumped first,
            // so a refusal of the runtime's state still leaves the SPIR-V.
            let budget = Budget::with_deadline(None);
            match dev.compile_device().compile_render_pipeline(&d, &budget) {
                Ok(p) => {
                    note(format!("{prefix} pipeline (compile layer, no draw variant):"));
                    dump_pipeline(dev, &p, out, prefix)?;
                }
                Err(err) => return Err(format!("compile_render_pipeline refused: {err} [{err:?}]")),
            }
            dev.new_render_pipeline_state(&d, &PipelineOptions::default())
                .map_err(|err| format!("render pipeline state: {err} [{err:?}]"))
        })();
        match attempt {
            Ok(x) => break x,
            Err(why) => {
                // `FunctionConstant { index: N }` in the error names what the compile wants.
                let wanted = if why.contains("NeedsInput(FunctionConstant") {
                    why.split("FunctionConstant { index: ")
                        .nth(1)
                        .and_then(|s| s.split(|c: char| !c.is_ascii_digit()).next())
                        .and_then(|s| s.parse::<u32>().ok())
                } else {
                    None
                };
                let Some(index) = wanted else { return Err(why) };
                let in_v = vdecl.iter().find(|d| d.index == Some(index)).cloned();
                let in_f = fvalues_decl.as_ref().and_then(|(_, d)| d.iter().find(|d| d.index == Some(index)).cloned());
                let which = in_v.clone().or(in_f.clone());
                let label = which
                    .as_ref()
                    .map(|d| format!("{} (#{index}, {})", d.name, data_type_name(d.ty)))
                    .unwrap_or(format!("#{index}"));
                note(format!("INSISTS: the compile wants constant {label}: {why}"));
                if insisted.contains(&label) || !spec.autofill_missing || insisted.len() > 64 {
                    return Err(why);
                }
                insisted.push(label);
                let ty = which.as_ref().and_then(|d| d.ty).unwrap_or(DataType::try_from(53u32).map_err(|_| "bool")?);
                let size = data_type_sizes(ty.as_raw()).map(|s| s[0]).unwrap_or(4);
                let value = FunctionConstantValue::new(ty, vec![0; size]);
                // Into each stage that declares it and was not given it, by name.
                if let Some(d) = &in_v {
                    if !vvalues.by_name.contains_key(&d.name) && !vvalues.by_index.contains_key(&index) {
                        vvalues.by_name.insert(d.name.clone(), value.clone());
                    }
                }
                if let (Some(d), Some((values, _))) = (&in_f, fvalues_decl.as_mut()) {
                    if !values.by_name.contains_key(&d.name) && !values.by_index.contains_key(&index) {
                        values.by_name.insert(d.name.clone(), value);
                    }
                }
            }
        }
    };
    if !insisted.is_empty() {
        note(format!("{prefix}: constants filled with zeros because the compile insisted: {}", insisted.join(", ")));
    }
    note(format!("{prefix}: render pipeline state made"));
    Ok(state)
}

/// A draw's resources, made once and bound on each replay of the pass.
#[derive(Default)]
struct Prepared {
    buffers: Vec<(char, u32, metal2vulkan_runtime::Buffer, u64)>,
    samplers: Vec<(char, u32, metal2vulkan_runtime::SamplerState)>,
    textures: Vec<(char, u32, Texture)>,
    index: Option<metal2vulkan_runtime::Buffer>,
}

fn prepare(dev: &Device, spec: &Spec, d: &Spec) -> R<Prepared> {
    let mut p = Prepared::default();
    for (&(stage, index), b) in &d.buffers {
        let at = b.data_at.unwrap_or(b.offset) as usize;
        let mut bytes = vec![0u8; (at + b.bytes.len()).max(b.length as usize).max(b.offset as usize + 16)];
        bytes[at..at + b.bytes.len()].copy_from_slice(&b.bytes);
        let buffer = dev.new_buffer_with_bytes(&bytes, ResourceOptions::shared()).map_err(e("buffer"))?;
        p.buffers.push((stage, index, buffer, b.offset));
    }
    for (&(stage, index), s) in &d.samplers {
        let mut sd = SamplerDescriptor::new();
        sd.min_filter = raw::<SamplerMinMagFilter>(s.min, "min filter")?;
        sd.mag_filter = raw::<SamplerMinMagFilter>(s.mag, "mag filter")?;
        sd.mip_filter = raw::<SamplerMipFilter>(s.mip, "mip filter")?;
        sd.s_address_mode = raw::<SamplerAddressMode>(s.s, "address")?;
        sd.t_address_mode = raw::<SamplerAddressMode>(s.t, "address")?;
        sd.r_address_mode = raw::<SamplerAddressMode>(s.r, "address")?;
        sd.border_color = raw::<SamplerBorderColor>(s.border, "border colour")?;
        sd.normalized_coordinates = s.normalized;
        sd.lod_min_clamp = s.lod_min;
        sd.lod_max_clamp = s.lod_max;
        p.samplers.push((stage, index, dev.new_sampler_state(&sd).map_err(e("sampler"))?));
    }
    for (&(stage, index), t) in &d.textures {
        let (texture, why) = make_texture(dev, t, &spec.astc)?;
        if let Some(why) = why {
            note(format!("NOTE draw {} texture {stage} {index}: {why}", d.label));
        }
        p.textures.push((stage, index, texture));
    }
    if let Some(spec::Draw::Indexed { offset, indices, .. }) = &d.draw {
        let mut bytes = vec![0u8; *offset as usize + indices.len()];
        bytes[*offset as usize..].copy_from_slice(indices);
        p.index = Some(dev.new_buffer_with_bytes(&bytes, ResourceOptions::shared()).map_err(e("index buffer"))?);
    }
    Ok(p)
}

/// The shader cache's render pipelines not seen yet, dumped under a prefix.
struct CacheScan {
    seen: std::collections::BTreeSet<String>,
    files: std::collections::BTreeSet<PathBuf>,
}

impl CacheScan {
    fn scan(&mut self, dev: &Device, cache: &Path, out: &Path, prefix: &str) -> R<usize> {
        let mut files = Vec::new();
        let mut stack = vec![cache.to_path_buf()];
        while let Some(dir) = stack.pop() {
            for entry in std::fs::read_dir(&dir).into_iter().flatten().flatten() {
                let p = entry.path();
                if p.is_dir() {
                    stack.push(p);
                } else if !self.files.contains(&p) {
                    files.push(p);
                }
            }
        }
        files.sort();
        let mut n = 0;
        for f in files {
            let Ok(bytes) = std::fs::read(&f) else { continue };
            if let Ok(p) = <RenderPipeline as PipelineArtifact>::from_bytes(&bytes, dev.compile_device()) {
                self.files.insert(f.clone());
                let digest = format!("{:?}", p.content_digest());
                if self.seen.insert(digest) {
                    let name = if n == 0 { prefix.to_string() } else { format!("{prefix}{n}") };
                    note(format!("pipeline from the shader cache ({name}, {}):", f.file_name().unwrap().to_string_lossy()));
                    dump_pipeline(dev, &p, out, &name)?;
                    n += 1;
                }
            }
        }
        Ok(n)
    }
}

struct Readback {
    index: u32,
    bytes: Vec<u8>,
}

type Late = (metal2vulkan_runtime::Buffer, u64, Vec<u8>, char, u32);

/// Encodes draws `0..upto` in one render pass on fresh attachments and reads them back.
fn run_pass(
    dev: &Device,
    spec: &Spec,
    states: &[Option<metal2vulkan_runtime::RenderPipelineState>],
    prepared: &[Prepared],
    upto: usize,
    mut scan: Option<(&mut CacheScan, &Path, &Path)>,
) -> R<Vec<Readback>> {
    let mut targets = Vec::new();
    for (&i, a) in &spec.attachments {
        let mut d = TextureDescriptor::texture_2d(raw::<PixelFormat>(a.format, "attachment format")?, a.width, a.height, false);
        d.usage = TextureUsage::RENDER_TARGET | TextureUsage::SHADER_READ;
        d.resource_options = ResourceOptions::private();
        targets.push((i, dev.new_texture(&d).map_err(e("attachment"))?));
    }
    let queue = dev.new_command_queue().map_err(e("queue"))?;
    let cb = queue.command_buffer().map_err(e("command buffer"))?;
    // Initial fills: a pass that only clears.
    if spec.attachments.values().any(|a| a.fill.is_some()) {
        let mut pass = RenderPassDescriptor::new();
        for (i, t) in &targets {
            let a = &spec.attachments[i];
            let c = &mut pass.color_attachments[*i as usize];
            c.texture = Some(t.clone());
            c.store_action = StoreAction::Store;
            if let Some(f) = a.fill {
                c.load_action = LoadAction::Clear;
                c.clear_color = ClearColor::new(f[0], f[1], f[2], f[3]);
            } else {
                c.load_action = LoadAction::Load;
            }
        }
        cb.render_command_encoder(&pass).map_err(e("fill pass"))?.end_encoding().map_err(e("fill pass end"))?;
    }
    let mut pass = RenderPassDescriptor::new();
    for (i, t) in &targets {
        let a = &spec.attachments[i];
        let c = &mut pass.color_attachments[*i as usize];
        c.texture = Some(t.clone());
        c.load_action = LoadAction::from_raw(a.load).map_err(e("load action"))?;
        c.store_action = StoreAction::from_raw(a.store).map_err(e("store action"))?;
        c.clear_color = ClearColor::new(a.clear[0], a.clear[1], a.clear[2], a.clear[3]);
    }
    if let Some((w, h)) = spec.rt {
        pass.render_target_width = w;
        pass.render_target_height = h;
    }
    let mut bound: std::collections::BTreeMap<(char, u32), metal2vulkan_runtime::Buffer> = Default::default();
    let mut late: Vec<Late> = Vec::new();
    let mut enc = cb.render_command_encoder(&pass).map_err(e("render encoder"))?;
    for k in 0..upto {
        let d = &spec.draws[k];
        let p = &prepared[k];
        let at = |what: &'static str| move |err: metal2vulkan_runtime::Error| format!("draw {}: {what}: {err:?}", d.label);
        // Metal's encoder state persists from draw to draw: each draw sets only what it names.
        if let Some(state) = &states[k] {
            enc.set_render_pipeline_state(state).map_err(at("set pipeline"))?;
        }
        for (stage, index, buffer, offset) in &p.buffers {
            if *stage == 'v' {
                enc.set_vertex_buffer(Some(buffer), *offset, *index).map_err(at("set vertex buffer"))?;
            } else {
                enc.set_fragment_buffer(Some(buffer), *offset, *index).map_err(at("set fragment buffer"))?;
            }
            bound.insert((*stage, *index), buffer.clone());
        }
        for (&(stage, index), bytes) in &d.bytes {
            if stage == 'v' {
                enc.set_vertex_bytes(bytes, index).map_err(at("set vertex bytes"))?;
            } else {
                enc.set_fragment_bytes(bytes, index).map_err(at("set fragment bytes"))?;
            }
        }
        for (stage, index, sampler) in &p.samplers {
            if *stage == 'v' {
                enc.set_vertex_sampler_state(Some(sampler), *index).map_err(at("set sampler"))?;
            } else {
                enc.set_fragment_sampler_state(Some(sampler), *index).map_err(at("set sampler"))?;
            }
        }
        for (stage, index, texture) in &p.textures {
            if *stage == 'v' {
                enc.set_vertex_texture(Some(texture), *index).map_err(at("set texture"))?;
            } else {
                enc.set_fragment_texture(Some(texture), *index).map_err(at("set texture"))?;
            }
        }
        if let Some(v) = d.viewport {
            enc.set_viewport(Viewport::new(v[0], v[1], v[2], v[3], v[4], v[5])).map_err(at("viewport"))?;
        }
        if let Some(s) = d.scissor {
            enc.set_scissor_rect(ScissorRect::new(s[0], s[1], s[2], s[3])).map_err(at("scissor"))?;
        }
        match &d.draw {
            Some(spec::Draw::Arrays { primitive, start, count, instances, base_instance }) => enc
                .draw_primitives(raw::<PrimitiveType>(*primitive, "primitive")?, *start, *count, *instances, *base_instance)
                .map_err(at("draw"))?,
            Some(spec::Draw::Indexed { primitive, count, index_type, offset, instances, base_vertex, base_instance, .. }) => enc
                .draw_indexed_primitives(
                    raw::<PrimitiveType>(*primitive, "primitive")?,
                    *count,
                    raw::<IndexType>(*index_type, "index type")?,
                    p.index.as_ref().unwrap(),
                    *offset,
                    *instances,
                    *base_vertex,
                    *base_instance,
                )
                .map_err(at("indexed draw"))?,
            None => note(format!("draw {}: no draw line; its state stays set for the next", d.label)),
        }
        for (stage, index, at_, data) in &d.late_patches {
            let b = bound.get(&(*stage, *index)).cloned().ok_or(format!("draw {}: late-patch of an unbound buffer", d.label))?;
            late.push((b, *at_, data.clone(), *stage, *index));
        }
        if let Some((scan, cache, out)) = scan.as_mut() {
            scan.scan(dev, cache, out, &format!("d{k}.drawn"))?;
        }
    }
    enc.end_encoding().map_err(e("end encoding"))?;
    for (b, at, data, stage, index) in &late {
        let contents = b.contents().ok_or("late-patch: no contents")?;
        if *at as usize + data.len() > b.length() as usize {
            return Err("late-patch past the buffer".into());
        }
        // SAFETY: inside the buffer; the command buffer is not committed yet.
        unsafe { std::ptr::copy_nonoverlapping(data.as_ptr(), contents.as_ptr().add(*at as usize), data.len()) };
        if scan.is_some() {
            note(format!("late-patch: wrote {} bytes at {at} of {stage} buffer {index} after encoding, before commit", data.len()));
        }
    }
    let mut readbacks = Vec::new();
    {
        let mut blit = cb.blit_command_encoder().map_err(e("blit"))?;
        for (i, t) in &targets {
            let a = &spec.attachments[i];
            let bpp = image::bytes_per_pixel(a.format).ok_or(format!("attachment {i}: no readback for format {}", a.format))?;
            let row = a.width * bpp;
            let rb = dev.new_buffer(row * a.height, ResourceOptions::shared()).map_err(e("readback"))?;
            blit.copy_from_texture_to_buffer(t, 0, 0, Origin::new(0, 0, 0), Size::new(a.width, a.height, 1), &rb, 0, row, row * a.height, BlitOption::None)
                .map_err(e("readback blit"))?;
            readbacks.push((*i, rb));
        }
        blit.end_encoding().map_err(e("blit end"))?;
    }
    cb.commit().map_err(e("commit"))?;
    cb.wait_until_completed_for(Duration::from_secs(300)).map_err(|_| "the command buffer did not complete in 300 s")?;
    match cb.status() {
        CommandBufferStatus::Completed => {}
        s => return Err(format!("command buffer ended {s:?}: {:?}", cb.error())),
    }
    let mut out = Vec::new();
    for (i, rb) in &readbacks {
        let contents = rb.contents().ok_or("readback without contents")?;
        // SAFETY: the buffer holds `length` bytes and the GPU is done with it.
        let bytes = unsafe { std::slice::from_raw_parts(contents.as_ptr(), rb.length() as usize) }.to_vec();
        out.push(Readback { index: *i, bytes });
    }
    Ok(out)
}

fn grid_region(spec: &Spec, a: &spec::Attachment) -> [u64; 4] {
    let r = spec.grid.unwrap_or_else(|| {
        let (w, h) = spec.rt.unwrap_or((a.width, a.height));
        [0, 0, w, h]
    });
    let (x, y) = (r[0].min(a.width), r[1].min(a.height));
    [x, y, r[2].min(a.width - x), r[3].min(a.height - y)]
}

fn print_grid(spec: &Spec, rb: &Readback, title: &str) -> R<Vec<Vec<[f64; 4]>>> {
    let a = &spec.attachments[&rb.index];
    let region = grid_region(spec, a);
    let grid = image::grid(a.format, a.width, a.height, &rb.bytes, region)?;
    println!(
        "{title}attachment {} ({}x{} format {}{}), 8x8 cell averages over x{} y{} {}x{}, as stored (channel order of the format), 0..255:",
        rb.index,
        a.width,
        a.height,
        a.format,
        if a.store == 0 { ", store=DontCare: what the driver left" } else { "" },
        region[0],
        region[1],
        region[2],
        region[3]
    );
    for row in &grid {
        let cells: Vec<String> = row.iter().map(|c| format!("{:3.0},{:3.0},{:3.0},{:3.0}", c[0], c[1], c[2], c[3])).collect();
        println!("  {}", cells.join(" | "));
    }
    Ok(grid)
}

fn run(spec_path: &Path, out: &Path) -> R<i32> {
    let text = std::fs::read_to_string(spec_path).map_err(|e| format!("read {}: {e}", spec_path.display()))?;
    let base = spec_path.parent().unwrap_or(Path::new("."));
    let spec = spec::parse(&text, base)?;
    std::fs::create_dir_all(out).map_err(|e| format!("mkdir {}: {e}", out.display()))?;
    // A fresh shader cache per run: what it holds afterwards is exactly this run's pipelines.
    let cache = out.join("shader-cache");
    let _ = std::fs::remove_dir_all(&cache);
    std::fs::create_dir_all(&cache).map_err(|e| format!("mkdir cache: {e}"))?;
    for f in std::fs::read_dir(out).map_err(|e| format!("{e}"))?.flatten() {
        let n = f.file_name().to_string_lossy().to_string();
        if n.ends_with(".spv") || n.ends_with(".spvasm") || n.ends_with(".png") || n.ends_with(".meta.txt") {
            let _ = std::fs::remove_file(f.path());
        }
    }
    let lib_path = spec.library.clone().ok_or("no library line")?;
    let lib = compile_library(&lib_path)?;
    note(format!("library: {} ({} functions)", lib_path.display(), lib.functions().count()));
    if spec.list_functions {
        let mut listing = String::new();
        for f in lib.functions() {
            let _ = writeln!(listing, "{} {:?} constants={}", f.name, f.stage, f.constants.len());
            for c in f.constants {
                let _ = writeln!(listing, "    {} index={:?} type={} flag={:?}", c.name, c.index, data_type_name(c.ty), c.raw_flag);
            }
        }
        std::fs::write(out.join("functions.txt"), &listing).map_err(|e| format!("{e}"))?;
        let names: Vec<String> = lib.functions().map(|f| format!("{}({:?})", f.name, f.stage)).collect();
        note(format!("functions ({}; constants in functions.txt): {}", names.len(), names.join(" ")));
    }
    if spec.draws.is_empty() {
        return Err("no draw: name at least a vertex function".into());
    }
    let dev = device(&spec, &cache)?;
    note(format!("device: {}", dev.name()));
    let library = dev.new_library_with_compile_library(lib.clone());
    let single = spec.draws.len() == 1;
    let mut scan = CacheScan { seen: Default::default(), files: Default::default() };
    let mut states = Vec::new();
    for (k, d) in spec.draws.iter().enumerate() {
        if d.vertex.is_some() {
            let prefix = if single { "static".to_string() } else { format!("d{k}.static") };
            states.push(Some(make_pipeline(&dev, &lib, &library, &spec, d, out, &prefix)?));
            scan.scan(&dev, &cache, out, &if single { "made".to_string() } else { format!("d{k}.made") })?;
        } else {
            if k == 0 {
                return Err("the first draw needs a pipeline (vertex line)".into());
            }
            states.push(None);
        }
    }
    let n = spec.draws.iter().rposition(|d| d.draw.is_some()).map(|i| i + 1).unwrap_or(0);
    if spec.compile_only || n == 0 {
        note("compile-only: no draw".into());
        scan.scan(&dev, &cache, out, "drawn")?;
        finish(out);
        return Ok(0);
    }
    let mut prepared = Vec::new();
    for d in &spec.draws {
        prepared.push(prepare(&dev, &spec, d)?);
    }
    if spec.prefixes && n > 1 {
        for upto in 1..n {
            let rbs = run_pass(&dev, &spec, &states, &prepared, upto, None)?;
            for rb in &rbs {
                print_grid(&spec, rb, &format!("after draw {} ({}), ", upto - 1, spec.draws[upto - 1].label))?;
            }
        }
    }
    let rbs = run_pass(&dev, &spec, &states, &prepared, n, Some((&mut scan, &cache, out)))?;
    let mut failed = false;
    for rb in &rbs {
        let a = &spec.attachments[&rb.index];
        let title = if single { String::new() } else { format!("after draw {} ({}), ", n - 1, spec.draws[n - 1].label) };
        let grid = print_grid(&spec, rb, &title)?;
        let rgba = image::to_rgba8(a.format, a.width, a.height, &rb.bytes)?;
        let png = out.join(format!("attachment{}.png", rb.index));
        image::write_png(&png, a.width as u32, a.height as u32, &rgba)?;
        if let Some((want, tol)) = a.expect {
            let worst = grid.iter().flatten().map(|c| (0..4).map(|k| (c[k] - want[k]).abs()).fold(0.0, f64::max)).fold(0.0, f64::max);
            let ok = worst <= tol;
            failed |= !ok;
            note(format!("{} attachment {}: expect {:?} ~{tol}, worst cell off by {worst:.1}", if ok { "PASS" } else { "FAIL" }, rb.index, want));
        }
        note(format!("wrote {}", png.display()));
    }
    drop(prepared);
    drop(states);
    scan.scan(&dev, &cache, out, "commit.drawn")?;
    finish(out);
    Ok(if failed { 3 } else { 0 })
}

fn finish(out: &Path) {
    let _ = std::fs::write(out.join("messages.txt"), MESSAGES.lock().unwrap().join("\n") + "\n");
    println!("output: {}", out.display());
}
