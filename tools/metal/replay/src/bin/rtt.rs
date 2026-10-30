//! A render pass to a texture, read back: a render pipeline state of a vertex and a fragment
//! function, a `Private` render target cleared to blue, two triangles over its top-left quarter,
//! and a blit of its texels into a `Shared` buffer the CPU reads.
//!
//! ```sh
//! cargo run -p metal2vulkan-runtime --example render_to_texture [-- path/to/library.metallib]
//! ```
//!
//! The functions are `colored_vertex` and `colored_fragment` of the repository's owned fixture
//! (`validation/fixtures/public/examples/runtime_examples.metal`), or of the metallib the argument
//! names. Positions are Metal's normalized device coordinates (y up, the texture's row 0 at the
//! top), whatever Vulkan's are: the runtime keeps Metal's.

#[path = "common/mod.rs"] mod common;

use metal2vulkan::metal::{PixelFormat, PrimitiveType, TextureUsage};
use metal2vulkan::pipeline::{ColorAttachmentDescriptor, RenderPipelineDescriptor, StageSource};
use metal2vulkan_runtime::{
    BlitOption, ClearColor, Device, LoadAction, Origin, PipelineOptions, RenderPassDescriptor,
    ResourceOptions, Size, StoreAction, TextureDescriptor,
};

/// The render target's side, in texels.
const SIDE: u64 = 8;

/// The color the triangles draw, and the bytes an `RGBA8Unorm` texel stores of it.
const DRAWN: [f32; 4] = [1.0, 0.2, 0.0, 1.0];
const DRAWN_TEXEL: [u8; 4] = [255, 51, 0, 255];

/// The clear color, and its texel.
const CLEARED_TEXEL: [u8; 4] = [0, 0, 255, 255];

fn main() -> common::Result<()> {
    let device = Device::system_default()?;
    println!("device: {}", device.name());
    let library = common::library(&device)?;
    let vertex = library.new_function_with_name("colored_vertex")?;
    let fragment = library.new_function_with_name("colored_fragment")?;

    // `MTLRenderPipelineDescriptor`: the compile layer's descriptor, its stages the functions'
    // descriptors. `newRenderPipelineStateWithDescriptor:error:` compiles both stages.
    let mut descriptor =
        RenderPipelineDescriptor::new(StageSource::Function(vertex.function_descriptor()));
    descriptor.fragment = Some(StageSource::Function(fragment.function_descriptor()));
    descriptor.color_attachments[0] = Some(ColorAttachmentDescriptor::new(PixelFormat::RGBA8Unorm));
    let state = device.new_render_pipeline_state(&descriptor, &PipelineOptions::default())?;

    // `texture2DDescriptorWithPixelFormat:width:height:mipmapped:`, a render target the GPU alone
    // reaches.
    let mut texture = TextureDescriptor::texture_2d(PixelFormat::RGBA8Unorm, SIDE, SIDE, false);
    texture.usage = TextureUsage::RENDER_TARGET;
    texture.resource_options = ResourceOptions::private();
    let target = device.new_texture(&texture)?;

    // The top-left quarter as two triangles, each vertex's position and color by vertex ID.
    let positions: [[f32; 2]; 6] = [
        [-1.0, 1.0],
        [0.0, 1.0],
        [-1.0, 0.0],
        [0.0, 1.0],
        [0.0, 0.0],
        [-1.0, 0.0],
    ];
    let positions: Vec<u8> = positions
        .iter()
        .flatten()
        .flat_map(|value| value.to_le_bytes())
        .collect();
    let colors: Vec<u8> = (0..6)
        .flat_map(|_| DRAWN)
        .flat_map(|value| value.to_le_bytes())
        .collect();
    let positions = device.new_buffer_with_bytes(&positions, ResourceOptions::shared())?;
    let colors = device.new_buffer_with_bytes(&colors, ResourceOptions::shared())?;

    // `MTLRenderPassDescriptor`: clear the target to blue, and store what the pass draws.
    let mut pass = RenderPassDescriptor::new();
    let color = &mut pass.color_attachments[0];
    color.texture = Some(target.clone());
    color.load_action = LoadAction::Clear;
    color.store_action = StoreAction::Store;
    color.clear_color = ClearColor::new(0.0, 0.0, 1.0, 1.0);

    let row = SIDE * 4;
    let readback = device.new_buffer(row * SIDE, ResourceOptions::shared())?;
    let queue = device.new_command_queue()?;
    let command_buffer = queue.command_buffer()?;
    let mut encoder = command_buffer.render_command_encoder(&pass)?;
    encoder.set_render_pipeline_state(&state)?;
    encoder.set_vertex_buffer(Some(&positions), 0, 0)?;
    encoder.set_vertex_buffer(Some(&colors), 0, 1)?;
    encoder.draw_primitives(PrimitiveType::Triangle, 0, 6, 1, 0)?;
    encoder.end_encoding()?;
    // `copyFromTexture:...toBuffer:...`: the blit runs after the pass, ordered by the runtime's
    // hazard tracking.
    let mut blit = command_buffer.blit_command_encoder()?;
    blit.copy_from_texture_to_buffer(
        &target,
        0,
        0,
        Origin::new(0, 0, 0),
        Size::new(SIDE, SIDE, 1),
        &readback,
        0,
        row,
        row * SIDE,
        BlitOption::None,
    )?;
    blit.end_encoding()?;
    common::run(&command_buffer)?;

    let texels = common::bytes(&readback)?;
    for y in 0..SIDE as usize {
        let mut line = String::new();
        for x in 0..SIDE as usize {
            let at = (y * SIDE as usize + x) * 4;
            let texel = &texels[at..at + 4];
            let inside = x < SIDE as usize / 2 && y < SIDE as usize / 2;
            let expected = if inside { DRAWN_TEXEL } else { CLEARED_TEXEL };
            if texel != expected {
                return Err(format!("texel ({x}, {y}) is {texel:?}, not {expected:?}").into());
            }
            line.push(if inside { '#' } else { '.' });
        }
        println!("{line}");
    }
    println!("every texel as expected: the top-left quarter drawn, the rest cleared");
    Ok(())
}
