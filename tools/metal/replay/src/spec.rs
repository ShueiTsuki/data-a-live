//! The draw spec: a plain line-based text file. One directive per line, `#` starts a comment
//! (only at the start of a line or after whitespace). See README-style help in `main.rs` (`USAGE`).

use std::collections::BTreeMap;
use std::path::PathBuf;

#[derive(Clone, Debug)]
pub struct Constant {
    /// `Some(name)` for `name:type:hex`, `None` for `#index:type:hex`.
    pub name: Option<String>,
    pub index: Option<u32>,
    pub ty: u32,
    pub bytes: Vec<u8>,
    /// The token as written.
    pub text: String,
}

#[derive(Clone, Debug, Default)]
pub struct Stage {
    pub name: String,
    pub constants: Vec<Constant>,
    /// Tokens that did not parse as constants (reported, skipped).
    pub broken: Vec<String>,
}

#[derive(Clone, Debug)]
pub struct Blend {
    pub enabled: bool,
    pub rgb: (u32, u32, u32),
    pub alpha: (u32, u32, u32),
}

#[derive(Clone, Debug)]
pub struct Attachment {
    pub format: u32,
    pub width: u64,
    pub height: u64,
    /// 0 dontcare, 1 load, 2 clear.
    pub load: u64,
    pub clear: [f64; 4],
    pub fill: Option<[f64; 4]>,
    pub blend: Blend,
    pub write_mask: u32,
    /// 0 dontcare, 1 store.
    pub store: u64,
    /// In the render pipeline descriptor's colour attachments (`pipeline=0`: the pass only).
    pub in_pipeline: bool,
    pub expect: Option<([f64; 4], f64)>,
}

#[derive(Clone, Debug)]
pub struct Attribute {
    pub format: u32,
    pub offset: u32,
    pub buffer: u32,
    pub stride: u32,
    pub step_function: u32,
    pub step_rate: u32,
}

#[derive(Clone, Debug)]
pub struct BufferSpec {
    pub length: u64,
    pub offset: u64,
    /// Where the bytes go in the buffer (`data=`), when not at the bound offset.
    pub data_at: Option<u64>,
    pub bytes: Vec<u8>,
}

#[derive(Clone, Debug)]
pub struct SamplerSpec {
    pub min: u32,
    pub mag: u32,
    pub mip: u32,
    pub s: u32,
    pub t: u32,
    pub r: u32,
    pub normalized: bool,
    pub lod_min: f32,
    pub lod_max: f32,
    pub border: u32,
}

#[derive(Clone, Debug)]
pub enum Fill {
    Solid([f64; 4]),
    AstcVoid([f64; 4]),
    /// Void-extent blocks: block columns `< cols` get `left`, the rest `right`.
    AstcVoidSplit { cols: u64, left: [f64; 4], right: [f64; 4] },
    File(PathBuf),
}

#[derive(Clone, Debug)]
pub struct TextureSpec {
    pub format: u32,
    pub width: u64,
    pub height: u64,
    pub fill: Fill,
}

#[derive(Clone, Debug)]
pub enum Draw {
    Arrays { primitive: u32, start: u32, count: u32, instances: u32, base_instance: u32 },
    Indexed {
        primitive: u32,
        count: u32,
        index_type: u32,
        indices: Vec<u8>,
        offset: u64,
        instances: u32,
        base_vertex: i32,
        base_instance: u32,
    },
}

#[derive(Clone, Debug, Default)]
pub struct Spec {
    /// Pass level: the draws in order (a single-draw spec has one). Each draw's own fields are
    /// the per-draw ones below; the pass-level ones are read from the top-level spec only.
    pub draws: Vec<Spec>,
    /// Per draw: the label of its `begin` / `== draw` line.
    pub label: String,
    /// Per draw: the pipeline's colour attachments (`color` lines); empty takes the pass's
    /// `attachment` lines that are in the pipeline.
    pub colors: BTreeMap<u32, Attachment>,
    /// Pass level: the render target size (`rt WxH`), when smaller than the attachments.
    pub rt: Option<(u64, u64)>,
    /// Pass level: the region the 8x8 grid averages (`grid x y w h`); default the render target.
    pub grid: Option<[u64; 4]>,
    /// Pass level: replay draws 0..k for every k, each in a fresh pass, and print each result.
    pub prefixes: bool,
    pub library: Option<PathBuf>,
    pub vertex: Option<Stage>,
    pub fragment: Option<Stage>,
    pub attachments: BTreeMap<u32, Attachment>,
    pub attributes: BTreeMap<u32, Attribute>,
    /// (stage 'v' or 'f', index)
    pub buffers: BTreeMap<(char, u32), BufferSpec>,
    pub bytes: BTreeMap<(char, u32), Vec<u8>>,
    pub samplers: BTreeMap<(char, u32), SamplerSpec>,
    pub textures: BTreeMap<(char, u32), TextureSpec>,
    pub draw: Option<Draw>,
    /// (stage, index, absolute offset, bytes): written into the bound buffer's contents after the
    /// draw is encoded and before commit, as a program may fill a shared buffer late.
    pub late_patches: Vec<(char, u32, u64, Vec<u8>)>,
    pub viewport: Option<[f64; 6]>,
    pub scissor: Option<[u64; 4]>,
    pub compile_only: bool,
    pub list_functions: bool,
    /// Supply zero bytes for each constant the compile insists on, and retry, reporting each.
    pub autofill_missing: bool,
    /// Drop given constants the function does not declare (default on).
    pub drop_unknown: bool,
    pub policy: String,
    /// `auto` (native, CPU-decoded RGBA8 stand-in where the device has no ASTC), `native`, `decode`.
    pub astc: String,
}

pub fn parse_hex(text: &str) -> Result<Vec<u8>, String> {
    let clean: String = text
        .trim_start_matches("0x")
        .chars()
        .filter(|c| *c != '_' && *c != ',' && !c.is_whitespace())
        .collect();
    if clean.len() % 2 != 0 {
        return Err(format!("odd number of hex digits in {text:?}"));
    }
    (0..clean.len())
        .step_by(2)
        .map(|i| u8::from_str_radix(&clean[i..i + 2], 16).map_err(|e| format!("hex {text:?}: {e}")))
        .collect()
}

/// `name:type:hex` or `#index:type:hex`.
pub fn parse_constant(token: &str) -> Result<Constant, String> {
    let parts: Vec<&str> = token.rsplitn(3, ':').collect();
    if parts.len() != 3 {
        return Err(format!("constant {token:?} is not name:type:hex"));
    }
    let (hex, ty, name) = (parts[0], parts[1], parts[2]);
    let ty: u32 = ty.parse().map_err(|_| format!("constant {token:?}: type {ty:?} is not a number"))?;
    let bytes = parse_hex(hex).map_err(|e| format!("constant {token:?}: {e}"))?;
    if bytes.is_empty() {
        return Err(format!("constant {token:?} has no bytes"));
    }
    let (name, index) = if let Some(i) = name.strip_prefix('#') {
        (None, Some(i.parse().map_err(|_| format!("constant {token:?}: bad index"))?))
    } else {
        if name.is_empty() {
            return Err(format!("constant {token:?} has no name"));
        }
        (Some(name.to_string()), None)
    };
    Ok(Constant { name, index, ty, bytes, text: token.to_string() })
}

fn floats(text: &str) -> Result<Vec<f64>, String> {
    text.split(',').map(|v| v.trim().parse::<f64>().map_err(|_| format!("number {v:?}"))).collect()
}

fn rgba(text: &str) -> Result<[f64; 4], String> {
    let v = floats(text)?;
    if v.len() != 4 {
        return Err(format!("{text:?} is not r,g,b,a"));
    }
    Ok([v[0], v[1], v[2], v[3]])
}

fn size(text: &str) -> Result<(u64, u64), String> {
    let (w, h) = text.split_once('x').ok_or(format!("size {text:?} is not WxH"))?;
    Ok((w.parse().map_err(|_| "width")?, h.parse().map_err(|_| "height")?))
}

fn stage_char(text: &str) -> Result<char, String> {
    match text {
        "v" | "vertex" => Ok('v'),
        "f" | "fragment" => Ok('f'),
        _ => Err(format!("stage {text:?} is not v or f")),
    }
}

fn num<T: std::str::FromStr>(text: &str, what: &str) -> Result<T, String> {
    let parsed = if let Some(hex) = text.strip_prefix("0x") {
        u64::from_str_radix(hex, 16).ok().and_then(|v| v.to_string().parse().ok())
    } else {
        text.parse().ok()
    };
    parsed.ok_or(format!("{what} {text:?} is not a number"))
}

fn named(text: &str, names: &[(&str, u32)], what: &str) -> Result<u32, String> {
    if let Ok(v) = text.parse() {
        return Ok(v);
    }
    names
        .iter()
        .find(|(n, _)| n.eq_ignore_ascii_case(text))
        .map(|(_, v)| *v)
        .ok_or(format!("{what} {text:?}: one of {:?} or a number", names.iter().map(|n| n.0).collect::<Vec<_>>()))
}

const FILTERS: &[(&str, u32)] = &[("nearest", 0), ("linear", 1)];
const MIPS: &[(&str, u32)] = &[("none", 0), ("notmipmapped", 0), ("nearest", 1), ("linear", 2)];
const ADDRESS: &[(&str, u32)] = &[
    ("clamp", 0),
    ("clamp_to_edge", 0),
    ("mirror_clamp", 1),
    ("repeat", 2),
    ("mirror", 3),
    ("mirror_repeat", 3),
    ("zero", 4),
    ("clamp_to_zero", 4),
    ("border", 5),
];
const PRIMITIVES: &[(&str, u32)] =
    &[("point", 0), ("line", 1), ("linestrip", 2), ("triangle", 3), ("trianglestrip", 4)];
const LOADS: &[(&str, u32)] = &[("dontcare", 0), ("load", 1), ("clear", 2)];
const OPS: &[(&str, u32)] = &[("add", 0), ("subtract", 1), ("reversesubtract", 2), ("min", 3), ("max", 4)];
const FACTORS: &[(&str, u32)] = &[
    ("zero", 0),
    ("one", 1),
    ("srccolor", 2),
    ("oneminussrccolor", 3),
    ("srcalpha", 4),
    ("oneminussrcalpha", 5),
    ("dstcolor", 6),
    ("oneminusdstcolor", 7),
    ("dstalpha", 8),
    ("oneminusdstalpha", 9),
];

/// Splits `key=value`; a bare word is its own key with an empty value.
fn kv(token: &str) -> (&str, &str) {
    token.split_once('=').unwrap_or((token, ""))
}

fn triple(text: &str, names: &[(&str, u32)], what: &str) -> Result<(u32, u32, u32), String> {
    let v: Vec<&str> = text.split(',').collect();
    if v.len() != 3 {
        return Err(format!("{what} {text:?} is not src,dst,op"));
    }
    Ok((named(v[0], FACTORS, "blend factor")?, named(v[1], FACTORS, "blend factor")?, named(v[2], names, "blend op")?))
}

/// `fill=solid:1,0,0,1`, or `fill=solid 1 0 0 1` (the numbers as the next four tokens).
fn fill(value: &str, rest: &mut std::iter::Peekable<std::slice::Iter<'_, &str>>) -> Result<Fill, String> {
    let (kind, inline) = value.split_once(':').unwrap_or((value, ""));
    let colour = |rest: &mut std::iter::Peekable<std::slice::Iter<'_, &str>>| -> Result<[f64; 4], String> {
        let mut v = [0.0; 4];
        for slot in &mut v {
            let t = rest.next().ok_or(format!("fill={kind} wants four numbers"))?;
            *slot = t.parse().map_err(|_| format!("fill={kind}: {t:?} is not a number"))?;
        }
        Ok(v)
    };
    let one = |rest: &mut std::iter::Peekable<std::slice::Iter<'_, &str>>| -> Result<[f64; 4], String> {
        if !inline.is_empty() { rgba(inline) } else { colour(rest) }
    };
    match kind {
        "solid" => Ok(Fill::Solid(one(rest)?)),
        "astc-void" => Ok(Fill::AstcVoid(one(rest)?)),
        "astc-void-split" => {
            let cols: u64 = rest.next().ok_or("astc-void-split wants cols r g b a r g b a")?.parse().map_err(|_| "astc-void-split cols")?;
            let left = colour(rest)?;
            let right = colour(rest)?;
            Ok(Fill::AstcVoidSplit { cols, left, right })
        }
        "file" => {
            if inline.is_empty() {
                Ok(Fill::File(PathBuf::from(rest.next().ok_or("fill=file wants a path")?)))
            } else {
                Ok(Fill::File(PathBuf::from(inline)))
            }
        }
        _ => Err(format!("fill {kind:?}: solid, astc-void, astc-void-split or file")),
    }
}

/// Strips a trailing comment: `#` at the line start or after whitespace, unless it begins a
/// `#index:type:hex` constant token.
fn strip_comment(line: &str) -> &str {
    let mut prev_space = true;
    for (i, c) in line.char_indices() {
        if c == '#' && prev_space {
            let rest = &line[i + 1..];
            let looks_indexed = rest.chars().next().map(|c| c.is_ascii_digit()).unwrap_or(false)
                && rest.split_whitespace().next().map(|t| t.contains(':')).unwrap_or(false);
            if !looks_indexed {
                return &line[..i];
            }
        }
        prev_space = c.is_whitespace();
    }
    line
}

fn new_attachment() -> Attachment {
    Attachment {
        format: 80,
        width: 64,
        height: 64,
        load: 2,
        clear: [0.0, 0.0, 0.0, 0.0],
        fill: None,
        blend: Blend { enabled: false, rgb: (1, 0, 0), alpha: (1, 0, 0) },
        write_mask: 15,
        store: 1,
        in_pipeline: true,
        expect: None,
    }
}

fn is_empty_draw(d: &Spec) -> bool {
    d.vertex.is_none() && d.fragment.is_none() && d.draw.is_none() && d.buffers.is_empty() && d.bytes.is_empty()
        && d.samplers.is_empty() && d.textures.is_empty() && d.viewport.is_none() && d.scissor.is_none()
        && d.colors.is_empty() && d.attributes.is_empty()
}

pub fn parse(text: &str, base: &std::path::Path) -> Result<Spec, String> {
    let mut spec = Spec { drop_unknown: true, policy: "default".into(), astc: "auto".into(), ..Spec::default() };
    let mut draws: Vec<Spec> = vec![Spec::default()];
    let resolve = |p: &str| -> PathBuf {
        let p = PathBuf::from(p);
        if p.is_absolute() { p } else { base.join(p) }
    };
    for (n, raw) in text.lines().enumerate() {
        let line = strip_comment(raw).trim();
        if line.is_empty() {
            continue;
        }
        let tokens: Vec<&str> = line.split_whitespace().collect();
        let at = |e: String| format!("line {}: {e}\n  {raw}", n + 1);
        let (head, args) = (tokens[0], &tokens[1..]);
        if head == "begin" || head == "==" {
            let mut d = Spec::default();
            d.label = args.iter().filter(|a| **a != "draw").copied().collect::<Vec<_>>().join(" ");
            if d.label.is_empty() {
                d.label = format!("{}", draws.len());
            }
            draws.push(d);
            continue;
        }
        (|| -> Result<(), String> {
            let cur = draws.last_mut().unwrap();
            match head {
                "library" => spec.library = Some(resolve(args.first().ok_or("library wants a path")?)),
                "vertex" | "fragment" | "vconst" | "fconst" => {
                    let is_vertex = head.starts_with('v');
                    let slot = if is_vertex { &mut cur.vertex } else { &mut cur.fragment };
                    let mut list = args.iter();
                    if head == "vertex" || head == "fragment" {
                        let name = list.next().ok_or(format!("{head} wants a function name"))?;
                        let stage = slot.get_or_insert_with(Stage::default);
                        stage.name = name.to_string();
                    }
                    let stage = slot.as_mut().ok_or(format!("{head} before its stage's function line"))?;
                    for token in list {
                        match parse_constant(token) {
                            Ok(c) => stage.constants.push(c),
                            Err(e) => stage.broken.push(e),
                        }
                    }
                }
                "attachment" | "color" => {
                    let index: u32 = num(args.first().ok_or("attachment wants an index")?, "attachment index")?;
                    let mut a = new_attachment();
                    for t in &args[1..] {
                        let (k, v) = kv(t);
                        match k {
                            "format" => a.format = num(v, "format")?,
                            "size" => (a.width, a.height) = size(v)?,
                            "load" => a.load = named(v, LOADS, "load action")? as u64,
                            "clear" => a.clear = rgba(v)?,
                            "fill" => a.fill = Some(rgba(v)?),
                            "blend" => a.blend.enabled = matches!(v, "on" | "1" | "true"),
                            "rgb" => {
                                a.blend.rgb = triple(v, OPS, "rgb blend")?;
                                a.blend.enabled = true
                            }
                            "alpha" => {
                                a.blend.alpha = triple(v, OPS, "alpha blend")?;
                                a.blend.enabled = true
                            }
                            "mask" => a.write_mask = num(v, "write mask")?,
                            "store" => a.store = named(v, &[("dontcare", 0), ("store", 1)], "store action")? as u64,
                            "pipeline" => a.in_pipeline = !matches!(v, "0" | "off" | "false"),
                            "expect" => {
                                let (c, tol) = v.split_once('~').unwrap_or((v, "8"));
                                a.expect = Some((rgba(c)?, tol.parse().map_err(|_| "expect tolerance")?));
                            }
                            _ => return Err(format!("attachment: unknown key {k:?}")),
                        }
                    }
                    if head == "color" {
                        cur.colors.insert(index, a);
                    } else {
                        spec.attachments.insert(index, a);
                    }
                }
                "attr" => {
                    for t in args {
                        let (k, v) = kv(t);
                        let index: u32 = num(k.trim_start_matches('a'), "attribute index")?;
                        let f: Vec<&str> = v.split(':').collect();
                        if f.len() < 4 {
                            return Err(format!("attr {t:?} is not aN=format:offset:buffer:stride"));
                        }
                        cur.attributes.insert(
                            index,
                            Attribute {
                                format: num(f[0], "vertex format")?,
                                offset: num(f[1], "offset")?,
                                buffer: num(f[2], "buffer index")?,
                                stride: num(f[3], "stride")?,
                                step_function: f.get(4).map(|s| num(s, "step function")).transpose()?.unwrap_or(1),
                                step_rate: f.get(5).map(|s| num(s, "step rate")).transpose()?.unwrap_or(1),
                            },
                        );
                    }
                }
                "buffer" => {
                    let stage = stage_char(args.first().ok_or("buffer wants v|f index")?)?;
                    let index: u32 = num(args.get(1).ok_or("buffer wants an index")?, "buffer index")?;
                    let mut b = BufferSpec { length: 0, offset: 0, data_at: None, bytes: Vec::new() };
                    for t in &args[2..] {
                        let (k, v) = kv(t);
                        match k {
                            "len" | "length" => b.length = num(v, "length")?,
                            "offset" => b.offset = num(v, "offset")?,
                            "data" => b.data_at = Some(num(v, "data")?),
                            "hex" => b.bytes.extend(parse_hex(v)?),
                            "file" => b.bytes.extend(std::fs::read(resolve(v)).map_err(|e| format!("{v}: {e}"))?),
                            "f32" => {
                                for x in floats(v)? {
                                    b.bytes.extend((x as f32).to_le_bytes())
                                }
                            }
                            _ => return Err(format!("buffer: unknown key {k:?}")),
                        }
                    }
                    cur.buffers.insert((stage, index), b);
                }
                "patch" | "late-patch" => {
                    // patch v|f <index> at=<absolute buffer offset> hex=.. | f32=..: overwrite bytes of a buffer line.
                    let stage = stage_char(args.first().ok_or("patch wants v|f index")?)?;
                    let index: u32 = num(args.get(1).ok_or("patch wants an index")?, "patch index")?;
                    let b = cur.buffers.get_mut(&(stage, index)).ok_or("patch before its buffer line")?;
                    let (mut at, mut data) = (b.offset, Vec::new());
                    for t in &args[2..] {
                        let (k, v) = kv(t);
                        match k {
                            "at" => at = num(v, "at")?,
                            "hex" => data.extend(parse_hex(v)?),
                            "f32" => {
                                for x in floats(v)? {
                                    data.extend((x as f32).to_le_bytes())
                                }
                            }
                            _ => return Err(format!("patch: unknown key {k:?}")),
                        }
                    }
                    if head == "late-patch" {
                        cur.late_patches.push((stage, index, at, data));
                        return Ok(());
                    }
                    let base = b.data_at.unwrap_or(b.offset);
                    if at < base {
                        return Err("patch below the buffer line's data".into());
                    }
                    let rel = (at - base) as usize;
                    if b.bytes.len() < rel + data.len() {
                        b.bytes.resize(rel + data.len(), 0);
                    }
                    b.bytes[rel..rel + data.len()].copy_from_slice(&data);
                }
                "bytes" => {
                    let stage = stage_char(args.first().ok_or("bytes wants v|f index hex")?)?;
                    let index: u32 = num(args.get(1).ok_or("bytes wants an index")?, "bytes index")?;
                    let mut bytes = Vec::new();
                    for t in &args[2..] {
                        if let Some(v) = t.strip_prefix("f32=") {
                            for x in floats(v)? {
                                bytes.extend((x as f32).to_le_bytes())
                            }
                        } else {
                            bytes.extend(parse_hex(t.trim_start_matches("hex="))?);
                        }
                    }
                    cur.bytes.insert((stage, index), bytes);
                }
                "sampler" => {
                    let stage = stage_char(args.first().ok_or("sampler wants v|f index")?)?;
                    let index: u32 = num(args.get(1).ok_or("sampler wants an index")?, "sampler index")?;
                    let mut s = SamplerSpec {
                        min: 0,
                        mag: 0,
                        mip: 0,
                        s: 0,
                        t: 0,
                        r: 0,
                        normalized: true,
                        lod_min: 0.0,
                        lod_max: f32::MAX,
                        border: 0,
                    };
                    for t in &args[2..] {
                        let (k, v) = kv(t);
                        match k {
                            "min" => s.min = named(v, FILTERS, "min filter")?,
                            "mag" => s.mag = named(v, FILTERS, "mag filter")?,
                            "filter" => {
                                s.min = named(v, FILTERS, "filter")?;
                                s.mag = s.min
                            }
                            "mip" => s.mip = named(v, MIPS, "mip filter")?,
                            "s" => s.s = named(v, ADDRESS, "address mode")?,
                            "t" => s.t = named(v, ADDRESS, "address mode")?,
                            "r" => s.r = named(v, ADDRESS, "address mode")?,
                            "address" => {
                                s.s = named(v, ADDRESS, "address mode")?;
                                s.t = s.s;
                                s.r = s.s
                            }
                            "norm" | "normalized" => s.normalized = matches!(v, "1" | "true" | "on"),
                            "lodmin" => s.lod_min = v.parse().map_err(|_| "lodmin")?,
                            "lodmax" => s.lod_max = v.parse().map_err(|_| "lodmax")?,
                            "border" => s.border = num(v, "border colour")?,
                            _ => return Err(format!("sampler: unknown key {k:?}")),
                        }
                    }
                    cur.samplers.insert((stage, index), s);
                }
                "texture" => {
                    let stage = stage_char(args.first().ok_or("texture wants v|f index")?)?;
                    let index: u32 = num(args.get(1).ok_or("texture wants an index")?, "texture index")?;
                    let (mut format, mut wh, mut f) = (70u32, (1u64, 1u64), Fill::Solid([1.0; 4]));
                    let mut rest = args[2..].iter().peekable();
                    while let Some(t) = rest.next() {
                        let (k, v) = kv(t);
                        match k {
                            "format" => format = num(v, "format")?,
                            "size" => wh = size(v)?,
                            "fill" => {
                                f = fill(v, &mut rest)?;
                                if let Fill::File(p) = &f {
                                    f = Fill::File(resolve(&p.to_string_lossy()));
                                }
                            }
                            _ => return Err(format!("texture: unknown key {k:?}")),
                        }
                    }
                    cur.textures.insert((stage, index), TextureSpec { format, width: wh.0, height: wh.1, fill: f });
                }
                "draw" | "drawindexed" => {
                    let primitive = named(args.first().ok_or("draw wants a primitive")?, PRIMITIVES, "primitive")?;
                    let mut m: BTreeMap<&str, &str> = BTreeMap::new();
                    for t in &args[1..] {
                        let (k, v) = kv(t);
                        m.insert(k, v);
                    }
                    let get = |k: &str, d: u64| -> Result<u64, String> { m.get(k).map(|v| num(v, k)).transpose().map(|v| v.unwrap_or(d)) };
                    cur.draw = Some(if head == "draw" {
                        Draw::Arrays {
                            primitive,
                            start: get("start", 0)? as u32,
                            count: get("count", 3)? as u32,
                            instances: get("instances", 1)? as u32,
                            base_instance: get("baseinstance", 0)? as u32,
                        }
                    } else {
                        let ty = match m.get("type").copied().unwrap_or("16") {
                            "16" | "uint16" | "0" => 0,
                            "32" | "uint32" | "1" => 1,
                            other => return Err(format!("index type {other:?}: 16 or 32")),
                        };
                        Draw::Indexed {
                            primitive,
                            count: get("count", 3)? as u32,
                            index_type: ty,
                            indices: parse_hex(m.get("indices").ok_or("drawindexed wants indices=hex")?)?,
                            offset: get("offset", 0)?,
                            instances: get("instances", 1)? as u32,
                            base_vertex: m.get("basevertex").map(|v| v.parse::<i32>().map_err(|_| "basevertex")).transpose()?.unwrap_or(0),
                            base_instance: get("baseinstance", 0)? as u32,
                        }
                    });
                }
                "viewport" => {
                    let v: Vec<f64> = args.iter().map(|a| a.parse().map_err(|_| format!("viewport {a:?}"))).collect::<Result<_, _>>()?;
                    if v.len() != 4 && v.len() != 6 {
                        return Err("viewport x y w h [znear zfar]".into());
                    }
                    cur.viewport = Some([v[0], v[1], v[2], v[3], *v.get(4).unwrap_or(&0.0), *v.get(5).unwrap_or(&1.0)]);
                }
                "scissor" => {
                    let v: Vec<u64> = args.iter().map(|a| num(a, "scissor")).collect::<Result<_, _>>()?;
                    if v.len() != 4 {
                        return Err("scissor x y w h".into());
                    }
                    cur.scissor = Some([v[0], v[1], v[2], v[3]]);
                }
                "compile-only" => spec.compile_only = true,
                "rt" => {
                    let v: Vec<&str> = args.iter().flat_map(|a| a.split('x')).collect();
                    if v.len() != 2 {
                        return Err("rt WxH".into());
                    }
                    spec.rt = Some((num(v[0], "rt width")?, num(v[1], "rt height")?));
                }
                "grid" => {
                    let v: Vec<u64> = args.iter().map(|a| num(a, "grid")).collect::<Result<_, _>>()?;
                    if v.len() != 4 {
                        return Err("grid x y w h".into());
                    }
                    spec.grid = Some([v[0], v[1], v[2], v[3]]);
                }
                "prefixes" => spec.prefixes = true,
                "list-functions" => spec.list_functions = true,
                "autofill-missing" => spec.autofill_missing = true,
                "keep-unknown" => spec.drop_unknown = false,
                "policy" => spec.policy = args.first().ok_or("policy default|exact")?.to_string(),
                "astc" => spec.astc = args.first().ok_or("astc auto|native|decode")?.to_string(),
                _ => return Err(format!("unknown directive {head:?}")),
            }
            Ok(())
        })()
        .map_err(at)?;
    }
    draws.retain(|d| !is_empty_draw(d));
    spec.draws = draws;
    Ok(spec)
}
