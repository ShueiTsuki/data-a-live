/* SPDX-License-Identifier: MIT */
#include "QcomUsbQualify.h"

static void clear_bytes(void *pointer, unsigned int size)
{
  unsigned char *bytes = (unsigned char *)pointer;
  while (size--) *bytes++ = 0;
}

static int supported_dwc3(QUSB_U32 id)
{
  /* Pinned kernel core.h: DWC3_IP=0x5533, DWC31_IP=0x3331. USB 3.2 and
   * DWC4 are not qualified by the current portable core backend.
   */
  return (id & 0xffff0000U) == 0x55330000U ||
         (id & 0xffff0000U) == 0x33310000U;
}

QUSB_RESULT QcomUsbInspectInherited(QUSB_READ32 read32, void *cookie,
                                   QUSB_INHERITED *state)
{
  static const QUSB_U32 cbcr[] = QUD_GCC_CBCR_OFFSETS;
  unsigned int i;
  if (!read32 || !state) return QusbBadArgument;
  clear_bytes(state, sizeof(*state));
  for (i = 0; i < 5; ++i) {
    if (!QUD_OFFSET_OK(cbcr[i], QUD_GCC_BYTES)) return QusbBadArgument;
    if (!read32(cookie, QUSB_GCC_BASE + cbcr[i], &state->gcc_cbcr[i]))
      return QusbReadFailed;
  }
  if (!read32(cookie, QUSB_GCC_BASE + QUD_GCC_BCR_OFFSET, &state->gcc_bcr))
    return QusbReadFailed;
  if (!read32(cookie, QUSB_GCC_BASE + QUD_GCC_GDSCR_OFFSET, &state->gcc_gdscr))
    return QusbReadFailed;
  if (!read32(cookie, QUSB_GCC_BASE + QUD_GCC_CFG_GDSCR_OFFSET, &state->gcc_cfg_gdscr))
    return QusbReadFailed;
#if QUD_GDSC_LEGACY_PWR_ON
  /* No qcom,support-cfg-gdscr on this block: PWR_ON is GDSCR bit31 and it is
   * stable once SW_COLLAPSE (bit0) has been cleared by the firmware. */
  if (!(state->gcc_gdscr & (1U << 31))) return QusbPowerDomainOff;
#else
  /* Live GDSC has qcom,support-cfg-gdscr. Its driver checks CFG_GDSCR
   * POWER_UP_COMPLETE bit16, not legacy GDSCR.PWR_ON bit31.
   */
  if (!(state->gcc_cfg_gdscr & (1U << 16))) return QusbPowerDomainOff;
#endif
  if (state->gcc_bcr & 1U) return QusbResetAsserted;
  /* CLK_OFF is bit 31 for clk_branch2. A voted clock can be running even
   * when this CPU's enable bit is zero, hence do not require enable bit 0.
   */
  for (i = 0; i < 5; ++i)
    if (state->gcc_cbcr[i] & 0x80000000U) return QusbClockOff;
  if (!read32(cookie, QUSB_DWC3_BASE + 0xc120, &state->gsnpsid))
    return QusbReadFailed;
  if (!supported_dwc3(state->gsnpsid))
    return QusbUnknownDwc3;
  if (!read32(cookie, QUSB_DWC3_BASE + 0xc110, &state->gctl) ||
      !read32(cookie, QUSB_DWC3_BASE + 0xc200, &state->gusb2phycfg) ||
      !read32(cookie, QUSB_DWC3_BASE + 0xc700, &state->dcfg) ||
      !read32(cookie, QUSB_DWC3_BASE + 0xc704, &state->dctl) ||
      !read32(cookie, QUSB_DWC3_BASE + 0xc70c, &state->dsts))
    return QusbReadFailed;
  return QusbQualified;
}

QUSB_RESULT QcomUsbQualifyIdentityDma(QUSB_READ32 read32, void *cookie,
                                    QUSB_SMMU_STATE *state)
{
  QUSB_U32 i, smr, s2cr, mask, id, type, page_shift, num_pages, cb, extended;
  QUSB_U64 cb_offset;
  QUSB_U32 low, high;
  if (!read32 || !state) return QusbBadArgument;
  clear_bytes(state, sizeof(*state));
  state->index = 0xffffffffU;
  if (!read32(cookie, QUSB_SMMU_BASE + 0x20, &state->id0) ||
      !read32(cookie, QUSB_SMMU_BASE + 0x24, &state->id1) ||
      !read32(cookie, QUSB_SMMU_BASE + 0x3c, &state->id7) ||
      !read32(cookie, QUSB_SMMU_BASE, &state->scr0))
    return QusbReadFailed;
  /* This backend implements SMMUv2 stream matching, not indexing. No ID
   * probing by temporary register writes.
   */
  /* ID7.MAJOR is the implementation revision (e.g. MMU-500 r2), not the
   * architecture version. The known qcom,qsmmu-v500 platform supplies the
   * SMMUv2 layout. Reject inaccessible/implausible ID registers rather than
   * falsely requiring implementation revision 2.
   */
  if (!(state->id0 & 0x60000000U) || !(state->id1 & 255U) ||
      (state->id1 & 255U) > 128U || (state->id0 & 255U) > 128U)
    return QusbUnknownSmmu;
  state->extended_stream_ids = (state->scr0 >> 3) & 1U;
  /* Arm IHI0062D.c 2.3.5: CLIENTPD bypasses the Stream mapping table and
   * address translation for the corresponding Security state. USFCFG only
   * affects unmatched streams when CLIENTPD=0. Reading the non-secure CR0
   * view for non-secure USB DMA is an explicit platform precondition.
   */
  if (state->scr0 & 1U) {
    state->mode = QusbDmaInheritedGlobalBypass;
    return QusbQualified;
  }
  if (!(state->id0 & (1U << 27))) return QusbUnknownSmmu;
  extended = state->extended_stream_ids;
  if (extended && !(state->id0 & (1U << 8))) return QusbExtendedStreamIds;
  state->extended_stream_ids = extended;
  state->stream_count = state->id0 & 255U;
  if (!state->stream_count) return QusbUnknownSmmu;
  for (i = 0; i < state->stream_count; ++i) {
    if (!read32(cookie, QUSB_SMMU_BASE + 0x800 + 4U * i, &smr))
      return QusbReadFailed;
    if (extended) {
      /* EXIDENABLE moves validity out of SMR[31] into S2CR[10]. The SMR
       * then has a full 16-bit mask and ID. In particular SMR[31] is mask
       * bit15, and cannot be interpreted as a legacy validity bit.
       */
      if (!read32(cookie, QUSB_SMMU_BASE + 0xc00 + 4U * i, &s2cr))
        return QusbReadFailed;
      if (!(s2cr & (1U << 10))) continue;
      mask = smr >> 16;
      id = smr & 0xffffU;
    } else {
      if (!(smr & 0x80000000U)) continue;
      mask = (smr >> 16) & 0x7fffU;
      id = smr & 0x7fffU;
    }
    if ((QUSB_STREAM_ID & ~mask) != (id & ~mask)) continue;
    if (!extended && !read32(cookie, QUSB_SMMU_BASE + 0xc00 + 4U * i, &s2cr))
      return QusbReadFailed;
    ++state->match_count;
    state->index = i;
    state->smr = smr;
    state->s2cr = s2cr;
    state->stream_id = id;
    state->stream_mask = mask;
  }
  if (!state->match_count) {
    /* IHI0062D.c 2.3.5/2.3.6: a proven unmatched non-secure stream bypasses
     * when NS CR0.USFCFG=0. No stream/global mutation or lease is needed.
     */
    if (!(state->scr0 & (1U << 10))) {
      state->mode = QusbDmaInheritedUnmatchedBypass;
      return QusbQualified;
    }
    return QusbNoExplicitStream;
  }
  if (state->match_count != 1U) return QusbMultipleStreams;
  type = (state->s2cr >> 16) & 3U;
  if (type == 1U) {
    state->mode = state->stream_mask ? QusbDmaInheritedSharedStreamBypass :
                                      QusbDmaExactStreamBypass;
    return QusbQualified;
  }
  if (type == 2U) return QusbStreamFaulting;
  if (type != 0U) return QusbUnsupportedStreamType;
  if (state->stream_mask) return QusbSharedStream;

  /* Read-only diagnostics for a translated stream. Never dereference its
   * page tables, change its context or pretend a fresh DMA allocation is
   * covered by the old bootloader IOVA mapping.
   */
  cb = state->s2cr & 255U;
  state->context_bank = cb;
  page_shift = (state->id1 & 0x80000000U) ? 16U : 12U;
  num_pages = 2U << ((state->id1 >> 28) & 7U);
  cb_offset = ((QUSB_U64)num_pages + cb) << page_shift;
  if (cb >= (state->id1 & 255U) || cb_offset + 0x34U > QUSB_SMMU_BYTES)
    return QusbUnknownSmmu;
  if (!read32(cookie, QUSB_SMMU_BASE + cb_offset, &state->context_sctlr) ||
      !read32(cookie, QUSB_SMMU_BASE + ((QUSB_U64)1U << page_shift) + 4U * cb,
              &state->context_cbar) ||
      !read32(cookie, QUSB_SMMU_BASE + cb_offset + 0x30, &state->context_tcr) ||
      !read32(cookie, QUSB_SMMU_BASE + cb_offset + 0x20, &low) ||
      !read32(cookie, QUSB_SMMU_BASE + cb_offset + 0x24, &high))
    return QusbReadFailed;
  state->context_ttbr0 = ((QUSB_U64)high << 32) | low;
  return QusbStreamTranslated;
}

static QUSB_RESULT core_halted(QUSB_READ32 read32, void *cookie)
{
  QUSB_U32 id, control, status, global_control;
  if (!read32(cookie, QUSB_DWC3_BASE + 0xc120, &id) ||
      !read32(cookie, QUSB_DWC3_BASE + 0xc110, &global_control) ||
      !read32(cookie, QUSB_DWC3_BASE + 0xc704, &control) ||
      !read32(cookie, QUSB_DWC3_BASE + 0xc70c, &status))
    return QusbReadFailed;
  if (!supported_dwc3(id)) return QusbUnknownDwc3;
  /* Device-side halted does not qualify an active host/OTG DMA engine. */
  if (((global_control >> 12) & 3U) != 2U) return QusbWrongCoreRole;
  if ((control & (1U << 31)) || !(status & (1U << 22)))
    return QusbCoreRunning;
  return QusbQualified;
}

static int valid_lease(const QUSB_DMA_LEASE *lease)
{
  if (!lease || lease->active != 1U || lease->new_entry > 1U ||
      !lease->stream_count || lease->stream_count > 128U ||
      lease->index >= lease->stream_count || lease->extended_stream_ids > 1U ||
      lease->smr != (lease->extended_stream_ids ? QUSB_STREAM_ID :
                                                (0x80000000U | QUSB_STREAM_ID)) ||
      (lease->original_cr0 & 9U) != (lease->extended_stream_ids << 3)) return 0;
  if (lease->new_entry) {
    if (lease->bypass_s2cr != (lease->extended_stream_ids ? 0x10700U : 0x10300U) ||
        !(lease->original_cr0 & (1U << 10))) return 0;
    if (lease->extended_stream_ids)
      return !(lease->original_s2cr & (1U << 10));
    return !(lease->original_smr & 0x80000000U);
  }
  return lease->original_smr == lease->smr &&
         (!lease->extended_stream_ids || (lease->original_s2cr & (1U << 10))) &&
         ((lease->original_s2cr >> 16) & 3U) == 0U &&
         lease->bypass_s2cr == ((lease->original_s2cr & ~(3U << 16)) | (1U << 16));
}

int QcomUsbDmaWriteAllowed(const QUSB_DMA_LEASE *lease,
                           QUSB_U64 address, QUSB_U32 value)
{
  QUSB_U64 smr_address, s2cr_address;
  if (!valid_lease(lease)) return 0;
  smr_address = QUSB_SMMU_BASE + 0x800 + 4U * lease->index;
  s2cr_address = QUSB_SMMU_BASE + 0xc00 + 4U * lease->index;
  if (!lease->new_entry) {
    if (lease->phase == QusbLeaseExistingCommit)
      return address == s2cr_address && value == lease->bypass_s2cr;
    if (lease->phase == QusbLeaseExistingRestore)
      return address == s2cr_address && value == lease->original_s2cr;
    return 0;
  }
  switch (lease->phase) {
    case QusbLeasePrepareS2cr: return address == s2cr_address && value == 0x10300U;
    case QusbLeasePrepareSmr: return address == smr_address && value == QUSB_STREAM_ID;
    case QusbLeaseCommit:
      return lease->extended_stream_ids ? address == s2cr_address && value == 0x10700U :
                                         address == smr_address && value == lease->smr;
    case QusbLeaseInvalidate:
      return lease->extended_stream_ids ? address == s2cr_address && value == 0x10300U :
                                         address == smr_address && value == QUSB_STREAM_ID;
    case QusbLeaseRestoreFirst:
      return lease->extended_stream_ids ? address == smr_address && value == lease->original_smr :
                                         address == s2cr_address && value == lease->original_s2cr;
    case QusbLeaseRestoreLast:
      return lease->extended_stream_ids ? address == s2cr_address && value == lease->original_s2cr :
                                         address == smr_address && value == lease->original_smr;
    default: return 0;
  }
}

static QUSB_RESULT lease_write(QUSB_READ32 read32, QUSB_WRITE32 write32,
                               QUSB_BARRIER barrier, void *cookie,
                               QUSB_DMA_LEASE *lease, QUSB_U32 phase,
                               QUSB_U64 address, QUSB_U32 value)
{
  QUSB_U32 check, cr0_mask = lease->new_entry ? 0x409U : 9U;
  int written;
  lease->phase = phase; /* A failed write may have committed: retain the phase. */
  if (!QcomUsbDmaWriteAllowed(lease, address, value)) return QusbBadArgument;
  if (!read32(cookie, QUSB_SMMU_BASE, &check)) return QusbReadFailed;
  if ((check & cr0_mask) != (lease->original_cr0 & cr0_mask)) return QusbOwnershipChanged;
  barrier(cookie);
  written = write32(cookie, address, value);
  /* Even a callback-reported failure may have posted the MMIO write. */
  barrier(cookie);
  lease->readback = 0xffffffffU;
  if (!written) return QusbWriteFailed;
  if (!read32(cookie, address, &check)) return QusbReadFailed;
  lease->readback = check;
  return check == value ? QusbQualified : QusbWriteFailed;
}

static QUSB_RESULT new_usb_entry(QUSB_READ32 read32, QUSB_WRITE32 write32,
                                 QUSB_BARRIER barrier, void *cookie,
                                 QUSB_DMA_LEASE *lease, QUSB_SMMU_STATE *state)
{
  QUSB_SMMU_STATE checked;
  QUSB_RESULT result;
  QUSB_U32 i, smr, s2cr, cr0, chosen = 0xffffffffU, old_smr = 0, old_s2cr = 0;
  QUSB_U64 smr_address, s2cr_address;
  result = QcomUsbQualifyIdentityDma(read32, cookie, &checked);
  if (result != QusbNoExplicitStream || checked.stream_count != state->stream_count ||
      (checked.scr0 & 0x409U) != (state->scr0 & 0x409U)) return QusbOwnershipChanged;
  for (i = 0; i < checked.stream_count; ++i) {
    if (!read32(cookie, QUSB_SMMU_BASE + 0x800 + 4U * i, &smr) ||
        !read32(cookie, QUSB_SMMU_BASE + 0xc00 + 4U * i, &s2cr)) return QusbReadFailed;
    if (checked.extended_stream_ids ? (s2cr & (1U << 10)) : (smr & 0x80000000U)) continue;
    chosen = i; old_smr = smr; old_s2cr = s2cr; break;
  }
  if (chosen == 0xffffffffU) return QusbNoFreeStream;
  smr_address = QUSB_SMMU_BASE + 0x800 + 4U * chosen;
  s2cr_address = QUSB_SMMU_BASE + 0xc00 + 4U * chosen;
  if (!read32(cookie, QUSB_SMMU_BASE, &cr0) ||
      !read32(cookie, smr_address, &smr) || !read32(cookie, s2cr_address, &s2cr))
    return QusbReadFailed;
  if ((cr0 & 0x409U) != (checked.scr0 & 0x409U) || smr != old_smr || s2cr != old_s2cr)
    return QusbOwnershipChanged;
  lease->index = chosen;
  lease->extended_stream_ids = checked.extended_stream_ids;
  lease->original_cr0 = checked.scr0;
  lease->original_smr = old_smr;
  lease->original_s2cr = old_s2cr;
  lease->stream_count = checked.stream_count;
  lease->smr = checked.extended_stream_ids ? QUSB_STREAM_ID : (0x80000000U | QUSB_STREAM_ID);
  /* Linux arm_smmu_write_s2cr: BYPASS, default privilege, non-shareable.
   * Configure validity last rather than enabling an old invalid SMR's SID.
   */
  lease->bypass_s2cr = checked.extended_stream_ids ? 0x10700U : 0x10300U;
  lease->new_entry = 1U;
  lease->active = 1U;
  result = lease_write(read32, write32, barrier, cookie, lease, QusbLeasePrepareS2cr,
                       s2cr_address, 0x10300U);
  if (result != QusbQualified) return result;
  result = lease_write(read32, write32, barrier, cookie, lease, QusbLeasePrepareSmr,
                       smr_address, QUSB_STREAM_ID);
  if (result != QusbQualified) return result;
  /* Both encodings are still invalid here. Rescan for a competing mapping
   * before the single validity commit. No unrelated entry is ever changed.
   */
  result = QcomUsbQualifyIdentityDma(read32, cookie, &checked);
  if (result != QusbNoExplicitStream || (checked.scr0 & 0x409U) != (lease->original_cr0 & 0x409U))
    return QusbOwnershipChanged;
  if (!read32(cookie, smr_address, &smr) || !read32(cookie, s2cr_address, &s2cr))
    return QusbReadFailed;
  if (smr != QUSB_STREAM_ID || s2cr != 0x10300U) return QusbOwnershipChanged;
  result = lease_write(read32, write32, barrier, cookie, lease, QusbLeaseCommit,
                       lease->extended_stream_ids ? s2cr_address : smr_address,
                       lease->extended_stream_ids ? lease->bypass_s2cr : lease->smr);
  if (result != QusbQualified) return result;
  result = QcomUsbQualifyIdentityDma(read32, cookie, state);
  if (result != QusbQualified || state->index != lease->index ||
      state->match_count != 1U || state->smr != lease->smr ||
      state->s2cr != lease->bypass_s2cr ||
      (state->scr0 & 0x409U) != (lease->original_cr0 & 0x409U)) return QusbOwnershipChanged;
  lease->phase = QusbLeaseComplete;
  return QusbQualified;
}

QUSB_RESULT QcomUsbEnterIdentityDma(QUSB_READ32 read32, QUSB_WRITE32 write32,
                                   QUSB_BARRIER barrier, void *cookie,
                                   QUSB_DMA_LEASE *lease,
                                   QUSB_SMMU_STATE *state)
{
  QUSB_RESULT result;
  QUSB_U32 check;
  if (!read32 || !write32 || !barrier || !lease || !state || lease->active)
    return QusbBadArgument;
  clear_bytes(lease, sizeof(*lease));
  result = QcomUsbQualifyIdentityDma(read32, cookie, state);
  if (result != QusbQualified && result != QusbStreamTranslated && result != QusbNoExplicitStream)
    return result;
  if (result == QusbStreamTranslated &&
      state->smr != (state->extended_stream_ids ? QUSB_STREAM_ID :
                                               (0x80000000U | QUSB_STREAM_ID)))
    return QusbUnknownSmmu;
  {
    QUSB_RESULT dma_result = result;
    result = core_halted(read32, cookie);
    if (result != QusbQualified) return result;
    if (dma_result == QusbQualified) return QusbQualified;
    if (dma_result == QusbNoExplicitStream)
      return new_usb_entry(read32, write32, barrier, cookie, lease, state);
  }
  /* Recheck this mapping immediately before changing it. Platform ownership
   * and disabled competing callbacks must be established by the caller.
   */
  if (!read32(cookie, QUSB_SMMU_BASE + 0x800 + 4U * state->index, &check))
    return QusbReadFailed;
  if (check != state->smr) return QusbOwnershipChanged;
  if (!read32(cookie, QUSB_SMMU_BASE + 0xc00 + 4U * state->index, &check))
    return QusbReadFailed;
  if (check != state->s2cr) return QusbOwnershipChanged;
  if (!read32(cookie, QUSB_SMMU_BASE, &check)) return QusbReadFailed;
  if ((check & 9U) != (state->scr0 & 9U)) return QusbOwnershipChanged;
  lease->index = state->index;
  lease->smr = state->smr;
  lease->original_s2cr = state->s2cr;
  lease->bypass_s2cr = (state->s2cr & ~(3U << 16)) | (1U << 16);
  lease->extended_stream_ids = state->extended_stream_ids;
  lease->original_cr0 = state->scr0;
  lease->original_smr = state->smr;
  lease->stream_count = state->stream_count;
  lease->active = 1U; /* A failed bus write can still have had a side effect. */
  result = lease_write(read32, write32, barrier, cookie, lease, QusbLeaseExistingCommit,
                       QUSB_SMMU_BASE + 0xc00 + 4U * lease->index, lease->bypass_s2cr);
  if (result != QusbQualified) return result;
  result = QcomUsbQualifyIdentityDma(read32, cookie, state);
  if ((state->scr0 & 9U) != (lease->original_cr0 & 9U)) return QusbOwnershipChanged;
  if (result == QusbQualified && (state->index != lease->index || state->match_count != 1U ||
      state->smr != lease->smr || state->s2cr != lease->bypass_s2cr)) return QusbOwnershipChanged;
  if (result == QusbQualified) lease->phase = QusbLeaseComplete;
  return result;
}

static int owned_new_pair(const QUSB_DMA_LEASE *lease, QUSB_U32 smr, QUSB_U32 s2cr)
{
  if (smr == lease->original_smr && s2cr == lease->original_s2cr) return 1;
  switch (lease->phase) {
    case QusbLeasePrepareS2cr:
      return smr == lease->original_smr && s2cr == 0x10300U;
    case QusbLeasePrepareSmr:
      return (smr == lease->original_smr || smr == QUSB_STREAM_ID) && s2cr == 0x10300U;
    case QusbLeaseCommit:
    case QusbLeaseInvalidate:
      return (smr == QUSB_STREAM_ID && s2cr == 0x10300U) ||
             (smr == lease->smr && s2cr == lease->bypass_s2cr);
    case QusbLeaseComplete:
      return smr == lease->smr && s2cr == lease->bypass_s2cr;
    case QusbLeaseRestoreFirst:
      return lease->extended_stream_ids ?
        (smr == QUSB_STREAM_ID || smr == lease->original_smr) && s2cr == 0x10300U :
        (smr == QUSB_STREAM_ID || smr == lease->original_smr) &&
          (s2cr == 0x10300U || s2cr == lease->original_s2cr);
    case QusbLeaseRestoreLast:
      return lease->extended_stream_ids ?
        smr == lease->original_smr && (s2cr == 0x10300U || s2cr == lease->original_s2cr) :
        (smr == QUSB_STREAM_ID || smr == lease->original_smr) && s2cr == lease->original_s2cr;
    default: return 0;
  }
}

static QUSB_RESULT restore_new_entry(QUSB_READ32 read32, QUSB_WRITE32 write32,
                                     QUSB_BARRIER barrier, void *cookie,
                                     QUSB_DMA_LEASE *lease,
                                     QUSB_U32 smr, QUSB_U32 s2cr)
{
  QUSB_U64 smr_address = QUSB_SMMU_BASE + 0x800 + 4U * lease->index;
  QUSB_U64 s2cr_address = QUSB_SMMU_BASE + 0xc00 + 4U * lease->index;
  QUSB_RESULT result;
  if (!owned_new_pair(lease, smr, s2cr)) return QusbOwnershipChanged;
  if (smr == lease->original_smr && s2cr == lease->original_s2cr) {
    lease->active = 0; lease->phase = QusbLeaseRestored; return QusbQualified;
  }
  /* Clear the active validity bit before restoring any old invalid fields. */
  if (lease->extended_stream_ids ? (s2cr & (1U << 10)) : (smr & 0x80000000U)) {
    result = lease_write(read32, write32, barrier, cookie, lease, QusbLeaseInvalidate,
                         lease->extended_stream_ids ? s2cr_address : smr_address,
                         lease->extended_stream_ids ? 0x10300U : QUSB_STREAM_ID);
    if (result != QusbQualified) return result;
  }
  result = lease_write(read32, write32, barrier, cookie, lease, QusbLeaseRestoreFirst,
                       lease->extended_stream_ids ? smr_address : s2cr_address,
                       lease->extended_stream_ids ? lease->original_smr : lease->original_s2cr);
  if (result != QusbQualified) return result;
  result = lease_write(read32, write32, barrier, cookie, lease, QusbLeaseRestoreLast,
                       lease->extended_stream_ids ? s2cr_address : smr_address,
                       lease->extended_stream_ids ? lease->original_s2cr : lease->original_smr);
  if (result != QusbQualified) return result;
  if (!read32(cookie, smr_address, &smr) || !read32(cookie, s2cr_address, &s2cr))
    return QusbReadFailed;
  if (smr != lease->original_smr || s2cr != lease->original_s2cr) return QusbOwnershipChanged;
  lease->active = 0; lease->phase = QusbLeaseRestored;
  return QusbQualified;
}

QUSB_RESULT QcomUsbRestoreDma(QUSB_READ32 read32, QUSB_WRITE32 write32,
                             QUSB_BARRIER barrier, void *cookie,
                             QUSB_DMA_LEASE *lease)
{
  QUSB_U32 smr, s2cr, cr0;
  QUSB_RESULT result;
  if (!read32 || !write32 || !barrier || !lease) return QusbBadArgument;
  if (!lease->active) return QusbQualified;
  if (!valid_lease(lease)) return QusbBadArgument;
  result = core_halted(read32, cookie);
  if (result != QusbQualified) return result;
  if (!read32(cookie, QUSB_SMMU_BASE, &cr0) ||
      !read32(cookie, QUSB_SMMU_BASE + 0x800 + 4U * lease->index, &smr) ||
      !read32(cookie, QUSB_SMMU_BASE + 0xc00 + 4U * lease->index, &s2cr))
    return QusbReadFailed;
  if ((cr0 & (lease->new_entry ? 0x409U : 9U)) !=
      (lease->original_cr0 & (lease->new_entry ? 0x409U : 9U))) return QusbOwnershipChanged;
  if (lease->new_entry)
    return restore_new_entry(read32, write32, barrier, cookie, lease, smr, s2cr);
  if (smr != lease->smr) return QusbOwnershipChanged;
  if (s2cr == lease->original_s2cr) {
    lease->active = 0;
    return QusbQualified;
  }
  if (s2cr != lease->bypass_s2cr) return QusbOwnershipChanged;
  result = lease_write(read32, write32, barrier, cookie, lease, QusbLeaseExistingRestore,
                       QUSB_SMMU_BASE + 0xc00 + 4U * lease->index, lease->original_s2cr);
  if (result != QusbQualified) return result;
  if (!read32(cookie, QUSB_SMMU_BASE + 0xc00 + 4U * lease->index, &s2cr))
    return QusbReadFailed;
  if (s2cr != lease->original_s2cr) return QusbWriteFailed;
  lease->active = 0;
  lease->phase = QusbLeaseRestored;
  return QusbQualified;
}

const char *QcomUsbResultName(QUSB_RESULT result)
{
  static const char *const names[] = {
    "qualified", "bad argument", "MMIO read failed", "inherited clock off",
    "USB reset asserted", "unknown DWC3 ID", "unknown SMMU layout",
    "global SMMU disabled", "extended stream IDs unsupported",
    "USB stream has no explicit mapping", "ambiguous USB stream mappings",
    "USB stream mapping covers other SIDs", "USB stream is translated",
    "USB stream faults", "unknown stream mapping type", "USB power domain off",
    "USB core is not halted", "USB S2CR write failed", "USB ownership changed",
    "USB core is not in device role", "USB power state requires a platform sequence",
    "USB inherited parent clock is not ready", "USB branch is forced off",
    "USB clock enable timed out", "no free USB stream entry"
  };
  if ((unsigned int)result >= sizeof(names) / sizeof(names[0])) return "unknown result";
  return names[result];
}
