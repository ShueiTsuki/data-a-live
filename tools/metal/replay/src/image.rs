//! Texel bytes: fills, ASTC void-extent blocks, readback conversion, PNG, the 8x8 grid.

use crate::spec::Fill;
use std::path::Path;

type R<T> = Result<T, String>;

const ASTC4X4_SRGB: u32 = 186;
const ASTC4X4_LDR: u32 = 204;

pub fn astc_is_srgb(format: u32) -> bool {
    (186..=199).contains(&format)
}

fn f16(v: f32) -> u16 {
    // Round-to-nearest-even float -> half, enough for fills.
    let bits = v.to_bits();
    let sign = ((bits >> 16) & 0x8000) as u16;
    let exp = ((bits >> 23) & 0xff) as i32 - 127 + 15;
    let mant = bits & 0x7f_ffff;
    if exp <= 0 {
        return sign;
    }
    if exp >= 31 {
        return sign | 0x7c00;
    }
    let mut h = (sign as u32) | ((exp as u32) << 10) | (mant >> 13);
    let rest = mant & 0x1fff;
    if rest > 0x1000 || (rest == 0x1000 && (h & 1) == 1) {
        h += 1;
    }
    h as u16
}

fn half_to_f32(h: u16) -> f32 {
    let sign = if h & 0x8000 != 0 { -1.0 } else { 1.0 };
    let exp = ((h >> 10) & 0x1f) as i32;
    let mant = (h & 0x3ff) as f32;
    sign * match exp {
        0 => mant * 2f32.powi(-24),
        31 => f32::INFINITY,
        _ => (1.0 + mant / 1024.0) * 2f32.powi(exp - 15),
    }
}

fn unorm8(v: f64) -> u8 {
    (v.clamp(0.0, 1.0) * 255.0).round() as u8
}

pub fn bytes_per_pixel(format: u32) -> Option<u64> {
    match format {
        10 | 11 => Some(1),
        70 | 71 | 80 | 81 => Some(4),
        115 => Some(8),
        125 => Some(16),
        _ => None,
    }
}

/// Rows of blocks (ASTC 4x4) or of texels.
pub fn block_rows(format: u32, height: u64) -> u64 {
    if format == ASTC4X4_LDR || format == ASTC4X4_SRGB { height.div_ceil(4) } else { height }
}

fn void_block(c: [f64; 4]) -> [u8; 16] {
    let mut b = [0u8; 16];
    b[..8].copy_from_slice(&[0xfc, 0xfd, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff]);
    for k in 0..4 {
        let v = (c[k].clamp(0.0, 1.0) * 65535.0).round() as u16;
        b[8 + 2 * k..10 + 2 * k].copy_from_slice(&v.to_le_bytes());
    }
    b
}

/// The bytes `replace_region` takes for a texture of `format` filled with `fill`, and the row pitch.
pub fn texture_bytes(format: u32, width: u64, height: u64, fill: &Fill) -> R<(Vec<u8>, u64)> {
    if format == ASTC4X4_LDR || format == ASTC4X4_SRGB {
        let row = width.div_ceil(4) * 16;
        let rows = height.div_ceil(4);
        return match fill {
            Fill::AstcVoid(c) => Ok(((0..row / 16 * rows).flat_map(|_| void_block(*c)).collect(), row)),
            Fill::AstcVoidSplit { cols, left, right } => {
                let bw = row / 16;
                Ok(((0..bw * rows).flat_map(|i| void_block(if i % bw < *cols { *left } else { *right })).collect(), row))
            }
            Fill::File(p) => {
                let b = std::fs::read(p).map_err(|e| format!("{}: {e}", p.display()))?;
                if (b.len() as u64) < row * rows {
                    return Err(format!("{}: {} bytes, the ASTC texture wants {}", p.display(), b.len(), row * rows));
                }
                Ok((b[..(row * rows) as usize].to_vec(), row))
            }
            Fill::Solid(_) => Err("an ASTC texture takes fill=astc-void or fill=file".into()),
        };
    }
    let bpp = bytes_per_pixel(format).ok_or(format!("texture format {format}: fill supports 10, 11, 70, 71, 80, 81, 115, 125 and ASTC 4x4 (186, 204)"))?;
    let row = width * bpp;
    let total = (row * height) as usize;
    match fill {
        Fill::Solid(c) => {
            let texel: Vec<u8> = match format {
                10 | 11 => vec![unorm8(c[0])],
                70 | 71 => vec![unorm8(c[0]), unorm8(c[1]), unorm8(c[2]), unorm8(c[3])],
                80 | 81 => vec![unorm8(c[2]), unorm8(c[1]), unorm8(c[0]), unorm8(c[3])],
                115 => c.iter().flat_map(|v| f16(*v as f32).to_le_bytes()).collect(),
                125 => c.iter().flat_map(|v| (*v as f32).to_le_bytes()).collect(),
                _ => unreachable!(),
            };
            Ok((texel.iter().copied().cycle().take(total).collect(), row))
        }
        Fill::File(p) => {
            let b = std::fs::read(p).map_err(|e| format!("{}: {e}", p.display()))?;
            if b.len() < total {
                return Err(format!("{}: {} bytes, the texture wants {total}", p.display(), b.len()));
            }
            Ok((b[..total].to_vec(), row))
        }
        Fill::AstcVoid(_) | Fill::AstcVoidSplit { .. } => Err(format!("fill=astc-void on non-ASTC format {format}")),
    }
}

/// An ASTC 4x4 fill decoded to RGBA8 on the CPU (void-extent blocks only).
pub fn astc_decode_fill(format: u32, width: u64, height: u64, fill: &Fill) -> R<Vec<u8>> {
    if format != ASTC4X4_LDR && format != ASTC4X4_SRGB {
        return Err(format!("CPU ASTC decode covers 4x4 (186, 204) only, not format {format}"));
    }
    let (blocks, row) = texture_bytes(format, width, height, fill)?;
    let bw = (row / 16) as usize;
    let mut out = vec![0u8; (width * height * 4) as usize];
    for y in 0..height as usize {
        for x in 0..width as usize {
            let at = ((y / 4) * bw + x / 4) * 16;
            let b = &blocks[at..at + 16];
            // A void-extent block: bits 0..8 = 0x1fc; bit 9 HDR.
            let header = u16::from_le_bytes([b[0], b[1]]);
            if header & 0x1ff != 0x1fc {
                return Err(format!("ASTC block ({}, {}) is not a void-extent block; the CPU stand-in decodes those only", x / 4, y / 4));
            }
            if header & 0x200 != 0 {
                return Err("an HDR void-extent block".into());
            }
            let o = (y * width as usize + x) * 4;
            for k in 0..4 {
                out[o + k] = (u16::from_le_bytes([b[8 + 2 * k], b[9 + 2 * k]]) >> 8) as u8;
            }
        }
    }
    Ok(out)
}

/// Each texel as 0..255 floats, in the format's own channel order.
fn texels(format: u32, width: u64, height: u64, bytes: &[u8]) -> R<Vec<[f64; 4]>> {
    let n = (width * height) as usize;
    let bpp = bytes_per_pixel(format).ok_or(format!("no readback for format {format}"))? as usize;
    if bytes.len() < n * bpp {
        return Err("short readback".into());
    }
    Ok((0..n)
        .map(|i| {
            let t = &bytes[i * bpp..(i + 1) * bpp];
            match format {
                10 | 11 => [t[0] as f64, 0.0, 0.0, 255.0],
                115 => {
                    let h = |k: usize| half_to_f32(u16::from_le_bytes([t[2 * k], t[2 * k + 1]])) as f64 * 255.0;
                    [h(0), h(1), h(2), h(3)]
                }
                125 => {
                    let f = |k: usize| f32::from_le_bytes([t[4 * k], t[4 * k + 1], t[4 * k + 2], t[4 * k + 3]]) as f64 * 255.0;
                    [f(0), f(1), f(2), f(3)]
                }
                _ => [t[0] as f64, t[1] as f64, t[2] as f64, t[3] as f64],
            }
        })
        .collect())
}

/// RGBA8 for the PNG (BGRA formats swapped to RGBA).
pub fn to_rgba8(format: u32, width: u64, height: u64, bytes: &[u8]) -> R<Vec<u8>> {
    let t = texels(format, width, height, bytes)?;
    let bgra = format == 80 || format == 81;
    Ok(t.iter()
        .flat_map(|c| {
            let c = if bgra { [c[2], c[1], c[0], c[3]] } else { *c };
            c.map(|v| v.clamp(0.0, 255.0).round() as u8)
        })
        .collect())
}

/// 8x8 cell averages, in stored channel order, 0..255.
pub fn grid(format: u32, width: u64, height: u64, bytes: &[u8], region: [u64; 4]) -> R<Vec<Vec<[f64; 4]>>> {
    let t = texels(format, width, height, bytes)?;
    let stride = width as usize;
    let (ox, oy) = (region[0] as usize, region[1] as usize);
    let (w, h) = (region[2].max(1) as usize, region[3].max(1) as usize);
    let mut out = vec![vec![[0.0; 4]; 8]; 8];
    for (gy, row) in out.iter_mut().enumerate() {
        for (gx, cell) in row.iter_mut().enumerate() {
            let (x0, x1) = (gx * w / 8, ((gx + 1) * w / 8).max(gx * w / 8 + 1).min(w));
            let (y0, y1) = (gy * h / 8, ((gy + 1) * h / 8).max(gy * h / 8 + 1).min(h));
            let mut sum = [0.0; 4];
            let mut n = 0.0;
            for y in y0..y1 {
                for x in x0..x1 {
                    for k in 0..4 {
                        sum[k] += t[(oy + y) * stride + ox + x][k];
                    }
                    n += 1.0;
                }
            }
            *cell = sum.map(|s| if n > 0.0 { s / n } else { 0.0 });
        }
    }
    Ok(out)
}

fn crc32(data: &[u8]) -> u32 {
    let mut c = 0xffff_ffffu32;
    for &b in data {
        c ^= b as u32;
        for _ in 0..8 {
            c = if c & 1 != 0 { 0xedb8_8320 ^ (c >> 1) } else { c >> 1 };
        }
    }
    !c
}

/// An RGBA8 PNG, zlib with stored (uncompressed) blocks.
pub fn write_png(path: &Path, width: u32, height: u32, rgba: &[u8]) -> R<()> {
    let mut raw = Vec::with_capacity((width as usize * 4 + 1) * height as usize);
    for y in 0..height as usize {
        raw.push(0);
        raw.extend_from_slice(&rgba[y * width as usize * 4..(y + 1) * width as usize * 4]);
    }
    let mut z = vec![0x78, 0x01];
    let chunks: Vec<&[u8]> = raw.chunks(65535).collect();
    for (i, c) in chunks.iter().enumerate() {
        z.push(if i + 1 == chunks.len() { 1 } else { 0 });
        z.extend((c.len() as u16).to_le_bytes());
        z.extend((!(c.len() as u16)).to_le_bytes());
        z.extend_from_slice(c);
    }
    let (mut a, mut b) = (1u32, 0u32);
    for &x in &raw {
        a = (a + x as u32) % 65521;
        b = (b + a) % 65521;
    }
    z.extend(((b << 16) | a).to_be_bytes());
    let mut png = vec![0x89, b'P', b'N', b'G', 0x0d, 0x0a, 0x1a, 0x0a];
    let mut chunk = |kind: &[u8], data: &[u8]| {
        png.extend((data.len() as u32).to_be_bytes());
        let mut body = kind.to_vec();
        body.extend_from_slice(data);
        png.extend_from_slice(&body);
        png.extend(crc32(&body).to_be_bytes());
    };
    let mut ihdr = Vec::new();
    ihdr.extend(width.to_be_bytes());
    ihdr.extend(height.to_be_bytes());
    ihdr.extend([8, 6, 0, 0, 0]);
    chunk(b"IHDR", &ihdr);
    chunk(b"IDAT", &z);
    chunk(b"IEND", &[]);
    std::fs::write(path, png).map_err(|e| format!("write {}: {e}", path.display()))
}
