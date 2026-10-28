/* SPDX-License-Identifier: MIT
 * Bypass-bank stream mapping, ported from the board's own UFS takeover
 * (drivers/NativeUfs/SmmuTakeover.h) and the sheng port (package/SmmuBypass.h).
 */
#include "QcomUsbBypass.h"

static void zero_bytes(void *p, QUSB_U32 n)
{
  unsigned char *b = p;
  while (n--) *b++ = 0;
}

static QUSB_U32 width_bits(QUSB_U32 code)
{
  static const QUSB_U32 bits[] = {32, 36, 40, 42, 44, 48};
  return code < 6u ? bits[code] : 0u;
}

static QUSB_U64 cb_address(const QUSB_BYPASS_LEASE *l)
{
  return QUSB_SMMU_BASE + (QUSB_U64)(l->pages + l->bank) * 4096u;
}

static QUSB_U64 cbar_address(const QUSB_BYPASS_LEASE *l)
{
  return QUSB_SMMU_BASE + 0x1000u + 4u * l->bank;
}

static QUSB_U64 cba2r_address(const QUSB_BYPASS_LEASE *l)
{
  return QUSB_SMMU_BASE + 0x1800u + 4u * l->bank;
}

static QUSB_U64 s2cr_address(const QUSB_BYPASS_LEASE *l)
{
  return QUSB_SMMU_BASE + 0xc00u + 4u * l->slot;
}

static QUSB_U64 smr_address(const QUSB_BYPASS_LEASE *l)
{
  return QUSB_SMMU_BASE + 0x800u + 4u * l->slot;
}

QUSB_RESULT QcomUsbBypassPlan(QUSB_READ32 read32, void *cookie, QUSB_U64 dma,
                              QUSB_U32 bytes, QUSB_BYPASS_LEASE *lease)
{
  QUSB_U32 id0, id1, id2, cr0, smr, s2cr, i, shift, bits, mask, id;
  QUSB_U32 streams, banks, pages, matches = 0;
  unsigned char used[128];
  QUSB_BYPASS_LEASE plan;

  if (!read32 || !lease || !bytes) return QusbBadArgument;
  zero_bytes(&plan, sizeof(plan));
  plan.slot = 0xffffffffU;
  plan.bank = 0xffffffffU;
  if (!read32(cookie, QUSB_SMMU_BASE + 0x20u, &id0) ||
      !read32(cookie, QUSB_SMMU_BASE + 0x24u, &id1) ||
      !read32(cookie, QUSB_SMMU_BASE + 0x28u, &id2) ||
      !read32(cookie, QUSB_SMMU_BASE, &cr0)) return QusbReadFailed;
  plan.original_cr0 = cr0;
  if (!(id0 & (1U << 30)) || !(id2 & (1U << 12))) return QusbUnknownSmmu;
  if (cr0 & 1u) {
    /* CLIENTPD: the whole non-secure SMMU is already bypassing. Nothing to do. */
    *lease = plan;
    return QusbQualified;
  }
  for (shift = 0; shift <= 8u; shift += 4u) {
    bits = width_bits((id2 >> shift) & 15u);
    if (!bits || dma >= (1ULL << bits) || (QUSB_U64)bytes > (1ULL << bits) - dma)
      return QusbUnknownSmmu;
  }
  streams = id0 & 255u;
  banks = id1 & 255u;
  pages = 1u << (((id1 >> 28) & 7u) + 1u);
  /* Data A Live: the register file has to fit the mapped window (SM8850: 128 global pages and
   * 81 banks, 0xd1000 bytes of a 0x100000 window), not nabu's 0xd0000. */
  if ((id1 >> 31) || !streams || streams > 128u || pages > 128u ||
      banks < 2u || banks > 128u || (QUSB_U64)(pages + banks) * 4096u > QUSB_SMMU_BYTES)
    return QusbUnknownSmmu;
  plan.pages = pages;

  /* Exactly one private match for our SID; any other state is left untouched. */
  for (i = 0; i < streams; ++i) {
    if (!read32(cookie, QUSB_SMMU_BASE + 0x800u + 4u * i, &smr)) return QusbReadFailed;
    if (!(smr & 0x80000000u)) continue;
    mask = (smr >> 16) & 0x7fffu;
    id = smr & 0x7fffu;
    if ((QUSB_STREAM_ID & ~mask) != (id & ~mask)) continue;
    ++matches;
    if (matches > 1u) return QusbMultipleStreams;
    if (mask) return QusbSharedStream;
    if (!read32(cookie, QUSB_SMMU_BASE + 0xc00u + 4u * i, &s2cr)) return QusbReadFailed;
    plan.slot = i;
    plan.original_smr = smr;
    plan.original_s2cr = s2cr;
  }
  if (plan.slot == 0xffffffffU) return QusbNoExplicitStream;
  if (((plan.original_s2cr >> 16) & 3u) == 1u) {
    /* This firmware never reports BYPASS as BYPASS, so treat it as done only
     * when the entry really holds type 1 (other implementations). */
    *lease = plan;
    return QusbQualified;
  }

  /* Banks referenced by another valid stream are off limits. */
  zero_bytes(used, sizeof(used));
  for (i = 0; i < streams; ++i) {
    if (i == plan.slot) continue;
    if (!read32(cookie, QUSB_SMMU_BASE + 0x800u + 4u * i, &smr)) return QusbReadFailed;
    if (!(smr & 0x80000000u)) continue;
    if (!read32(cookie, QUSB_SMMU_BASE + 0xc00u + 4u * i, &s2cr)) return QusbReadFailed;
    if (((s2cr >> 16) & 3u) == 0u && (s2cr & 0xffu) < banks) used[s2cr & 0xffu] = 1;
  }
  /* Prefer the bank our entry already selects, then the top bank downwards. */
  for (i = 0; i < banks; ++i) {
    QUSB_U32 candidate = i == 0u ? (plan.original_s2cr & 0xffu) : (banks - i);
    QUSB_U32 sctlr;
    if (candidate >= banks || used[candidate]) continue;
    if (!read32(cookie, QUSB_SMMU_BASE + (QUSB_U64)(pages + candidate) * 4096u, &sctlr))
      return QusbReadFailed;
    if (sctlr & 1u) continue;   /* translation enabled: live bank */
    plan.bank = candidate;
    break;
  }
  if (plan.bank >= banks) return QusbNoFreeStream;

  /* Save the bank state and decide the minimum set of writes. */
  if (!read32(cookie, cb_address(&plan), &plan.saved_sctlr) ||
      !read32(cookie, cbar_address(&plan), &plan.saved_cbar) ||
      !read32(cookie, cba2r_address(&plan), &plan.saved_cba2r)) return QusbReadFailed;
  plan.changed = 0;
  /* Data A Live: what passes the stream untranslated is SCTLR.M clear (a candidate's already
   * is) and CBAR.TYPE 1, S1_TRANS_S2_BYPASS; the bank SM8850's bootloader leaves for stream
   * 0x40 has SCTLR 0xe0 (fault reporting bits) and CBAR 0x1f000 (bypass memory attributes
   * in bits 15:12), and is used as it is rather than rewritten to 0 and 0x10000. */
  if (plan.saved_sctlr & 1u) plan.changed |= 1u;
  if (((plan.saved_cbar >> 16) & 3u) != 1u) plan.changed |= 2u;
  if (plan.saved_cba2r != 1u) plan.changed |= 4u;
  if ((plan.original_s2cr & 0xffu) != plan.bank || ((plan.original_s2cr >> 16) & 3u) != 0u)
    plan.changed |= 8u;
  plan.active = plan.changed ? 1u : 0u;
  plan.phase = plan.active ? 1u : 0u;   /* commit phase until Restore */
  *lease = plan;
  return QusbQualified;
}

QUSB_RESULT QcomUsbBypassEnter(QUSB_READ32 read32, QUSB_WRITE32 write32,
                               QUSB_BARRIER barrier, void *cookie,
                               QUSB_BYPASS_LEASE *lease)
{
  if (!read32 || !write32 || !barrier || !lease) return QusbBadArgument;
  if (!lease->active) return QusbQualified;
  lease->phase = 1u;
  barrier(cookie);
  if ((lease->changed & 1u) && !write32(cookie, cb_address(lease), 0u)) return QusbWriteFailed;
  if ((lease->changed & 4u) && !write32(cookie, cba2r_address(lease), 1u)) return QusbWriteFailed;
  if ((lease->changed & 2u) && !write32(cookie, cbar_address(lease), 0x10000u)) return QusbWriteFailed;
  barrier(cookie);
  if ((lease->changed & 8u) && !write32(cookie, s2cr_address(lease), lease->bank))
    return QusbWriteFailed;
  if (!(lease->original_smr & 0x80000000u) &&
      !write32(cookie, smr_address(lease), 0x80000000u | QUSB_STREAM_ID))
    return QusbWriteFailed;
  barrier(cookie);
  lease->readback = 0u;
  (void)read32(cookie, s2cr_address(lease), &lease->readback);
  return QusbQualified;
}

QUSB_RESULT QcomUsbBypassRestore(QUSB_READ32 read32, QUSB_WRITE32 write32,
                                 QUSB_BARRIER barrier, void *cookie,
                                 QUSB_BYPASS_LEASE *lease)
{
  if (!read32 || !write32 || !barrier || !lease) return QusbBadArgument;
  if (!lease->active) return QusbQualified;
  lease->phase = 2u;
  barrier(cookie);
  if ((lease->changed & 8u) && !write32(cookie, s2cr_address(lease), lease->original_s2cr))
    return QusbWriteFailed;
  if ((lease->changed & 1u) && !write32(cookie, cb_address(lease), lease->saved_sctlr))
    return QusbWriteFailed;
  if ((lease->changed & 2u) && !write32(cookie, cbar_address(lease), lease->saved_cbar))
    return QusbWriteFailed;
  if ((lease->changed & 4u) && !write32(cookie, cba2r_address(lease), lease->saved_cba2r))
    return QusbWriteFailed;
  barrier(cookie);
  lease->active = 0u;
  return QusbQualified;
}

int QcomUsbBypassWriteAllowed(const QUSB_BYPASS_LEASE *lease,
                              QUSB_U64 address, QUSB_U32 value)
{
  if (!lease || !lease->active || lease->bank >= 128u) return 0;
  if (lease->phase == 1u) {
    if ((lease->changed & 1u) && address == cb_address(lease) && value == 0u) return 1;
    if ((lease->changed & 4u) && address == cba2r_address(lease) && value == 1u) return 1;
    if ((lease->changed & 2u) && address == cbar_address(lease) && value == 0x10000u) return 1;
    if ((lease->changed & 8u) && address == s2cr_address(lease) && value == lease->bank) return 1;
    if (!(lease->original_smr & 0x80000000u) && address == smr_address(lease) &&
        value == (0x80000000u | QUSB_STREAM_ID)) return 1;
    return 0;
  }
  if (lease->phase == 2u) {
    if ((lease->changed & 8u) && address == s2cr_address(lease) &&
        value == lease->original_s2cr) return 1;
    if ((lease->changed & 1u) && address == cb_address(lease) &&
        value == lease->saved_sctlr) return 1;
    if ((lease->changed & 2u) && address == cbar_address(lease) &&
        value == lease->saved_cbar) return 1;
    if ((lease->changed & 4u) && address == cba2r_address(lease) &&
        value == lease->saved_cba2r) return 1;
  }
  return 0;
}
