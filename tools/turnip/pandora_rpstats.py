#!/usr/bin/env python3
"""Turnip (Mesa 85fda3b with UniXNU's patches) counts how it draws each render pass, for the Metal plugin's log
(tools/metal/adapt_ios16.py): in on-chip tiles (GMEM: passes, tiles drawn, passes with hardware binning, and
passes that store an attachment neither loaded nor cleared, render area whole or part: r1c showed a widget
garbled for a while once GMEM was allowed, and such a store is where GMEM and sysmem differ) or
straight to memory (sysmem) with the reason Turnip gives in gmem_disable_reason ("Autotune selected sysmem",
"TU_DEBUG(SYSMEM)", ...). tu_pandora_render_stats(out, size) writes the totals as one line. Counting only:
which path a pass takes is still Turnip's own choice (use_sysmem_rendering and its autotune).

  pandora_rpstats.py MESA_DIR

Exact text edits, asserted; run once on a patched tree (local: tools/turnip/build_ios_linux.sh; CI: turnip.yml).
"""
import pathlib
import sys

p = pathlib.Path(sys.argv[1]) / 'src/freedreno/vulkan/tu_cmd_buffer.cc'
s = p.read_text()
for old, new in (
        ('''template <chip CHIP>
void
tu_cmd_render(struct tu_cmd_buffer *cmd_buffer,
              const VkOffset2D *fdm_offsets)
{''', '''/* data-a-live (tools/turnip/pandora_rpstats.py): how render passes are drawn, for the Metal plugin's log. */
static struct {
   uint64_t gmem, tiles, binned, sysmem;
   uint64_t undef_full, undef_part;   /* GMEM passes storing an attachment neither loaded nor cleared */
   const char *reason[8];
   uint64_t by_reason[8];
} pandora_rp;

static void
pandora_rp_note(struct tu_cmd_buffer *cmd, bool sysmem)
{
   if (!sysmem) {
      const struct tu_vsc_config *vsc = tu_vsc_config(cmd, cmd->state.tiling);
      p_atomic_inc(&pandora_rp.gmem);
      p_atomic_add(&pandora_rp.tiles, (uint64_t)vsc->tile_count.width * vsc->tile_count.height);
      if (use_hw_binning(cmd))
         p_atomic_inc(&pandora_rp.binned);
      /* An attachment stored but neither loaded nor cleared: in GMEM its pixels no draw reaches are
       * stored from whatever the tile held; in sysmem they keep what memory held. */
      const struct tu_render_pass *pass = cmd->state.pass;
      const struct tu_framebuffer *fb = cmd->state.framebuffer;
      bool undef = false;
      for (uint32_t i = 0; pass && i < pass->attachment_count; i++) {
         const struct tu_render_pass_attachment *att = &pass->attachments[i];
         if (att->gmem && att->store && !att->load && !att->clear_mask)
            undef = true;
      }
      if (undef) {
         const VkRect2D *ra = &cmd->state.render_areas[0];
         const bool full = fb && ra->offset.x == 0 && ra->offset.y == 0 &&
                           ra->extent.width >= fb->width && ra->extent.height >= fb->height;
         p_atomic_inc(full ? &pandora_rp.undef_full : &pandora_rp.undef_part);
      }
      return;
   }
   p_atomic_inc(&pandora_rp.sysmem);
   const char *why = cmd->state.rp.gmem_disable_reason ? cmd->state.rp.gmem_disable_reason : "no reason given";
   for (unsigned i = 0; i < ARRAY_SIZE(pandora_rp.reason); i++) {
      const char *seen = (const char *)p_atomic_cmpxchg_ptr(&pandora_rp.reason[i], (const char *)NULL, why);
      if (seen == NULL || seen == why) {
         p_atomic_inc(&pandora_rp.by_reason[i]);
         return;
      }
   }
}

extern "C" __attribute__((visibility("default"))) size_t tu_pandora_render_stats(char *out, size_t size);
extern "C" __attribute__((visibility("default"))) size_t
tu_pandora_render_stats(char *out, size_t size)
{
   int at = snprintf(out, size, "gmem %llu (tiles %llu, hw binning %llu, undefined store full %llu part %llu) sysmem %llu",
                     (unsigned long long)p_atomic_read(&pandora_rp.gmem),
                     (unsigned long long)p_atomic_read(&pandora_rp.tiles),
                     (unsigned long long)p_atomic_read(&pandora_rp.binned),
                     (unsigned long long)p_atomic_read(&pandora_rp.undef_full),
                     (unsigned long long)p_atomic_read(&pandora_rp.undef_part),
                     (unsigned long long)p_atomic_read(&pandora_rp.sysmem));
   for (unsigned i = 0; i < ARRAY_SIZE(pandora_rp.reason) && at > 0 && (size_t)at < size; i++) {
      const char *why = (const char *)p_atomic_read(&pandora_rp.reason[i]);
      if (why)
         at += snprintf(out + at, size - at, " | %s: %llu", why,
                        (unsigned long long)p_atomic_read(&pandora_rp.by_reason[i]));
   }
   return at > 0 ? (size_t)at : 0;
}

template <chip CHIP>
void
tu_cmd_render(struct tu_cmd_buffer *cmd_buffer,
              const VkOffset2D *fdm_offsets)
{'''),
        ('''   if (use_sysmem_rendering(cmd_buffer, &rp_ctx, rp_key))
      tu_cmd_render_sysmem<CHIP>(cmd_buffer, rp_ctx);
   else
      tu_cmd_render_tiles<CHIP>(cmd_buffer, rp_ctx, fdm_offsets);''', '''   if (use_sysmem_rendering(cmd_buffer, &rp_ctx, rp_key)) {
      pandora_rp_note(cmd_buffer, true);
      tu_cmd_render_sysmem<CHIP>(cmd_buffer, rp_ctx);
   } else {
      pandora_rp_note(cmd_buffer, false);
      tu_cmd_render_tiles<CHIP>(cmd_buffer, rp_ctx, fdm_offsets);
   }'''),
):
    assert s.count(old) == 1, old[:60]
    s = s.replace(old, new)
p.write_text(s)
print('edited', p)
