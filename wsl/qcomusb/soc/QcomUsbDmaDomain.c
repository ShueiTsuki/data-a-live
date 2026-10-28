/* SPDX-License-Identifier: MIT
 * Private AArch64 SMMUv2 S1 domain: exactly eight RW/NX/nG pages, no bypass.
 */
#include "QcomUsbDmaDomain.h"

#define MAGIC 0x51534431U
#define PA_MASK 0x0000fffffffff000ULL
#define LEAF_FLAGS 0x0060000000000f43ULL
#define SCTLR_VALUE 0x27U /* CFRE, AFE, TRE, M; optional ASIDPNE hint remains zero. */
#define TCR2_CRITICAL 0x38017U /* SEP, ASID width and output address size. */
#define FSR_FAULT 0xc00001feU /* Linux: MULTI/SS/UUT/ASF/TLBLKF/TLBMCF/EF/PF/AFF/TF. */
#define FSR_FORMAT 0x600U     /* Read-only status, generated from VA64/effective EAE. */
#define FSR_RESERVED (~(FSR_FAULT|FSR_FORMAT))
enum { CFG=1, TLBI=2, SYNC=3, ENABLE=4, S2PREP=5, SMRPREP=6, COMMIT=7,
       LIVE=8, INVALIDATE=9, DISABLE=10, RESTORE_CFG=11, PAIR_FIRST=12, PAIR_LAST=13 };

static void zero(void *p,QUSB_U32 n) { unsigned char *b=p; while(n--) *b++=0; }
static QUSB_U32 physical_bits(QUSB_U32 code)
{
  static const QUSB_U32 bits[]={32,36,40,42,44,48};
  return code<6?bits[code]:0;
}
static int fsr_idle(QUSB_U32 value)
{
  /* FORMAT2=0x400 is normal AArch64 status, not a recorded fault. Unknown
   * reserved bits remain a refusal rather than being labelled a fault. */
  return !(value&FSR_FAULT)&&!(value&FSR_RESERVED);
}
static int fsr_a64(QUSB_U32 value)
{
  return fsr_idle(value)&&(value&FSR_FORMAT)==0x400U;
}
static int extent(QUSB_U64 pa,QUSB_U32 n,QUSB_U32 bits)
{
  return bits>=32 && bits<=48 && !(pa&4095U) && pa && pa+n>pa && pa+n<=(1ULL<<bits);
}

QUSB_DOMAIN_RESULT QcomUsbDomainBuildTables(void *cpu,QUSB_U64 pa,QUSB_U32 bytes,
                                           QUSB_U64 dma,QUSB_U32 dma_bytes,QUSB_U32 bits)
{
  QUSB_U64 *p=cpu;
  QUSB_U32 i;
  if(!cpu || ((QUSB_U64)cpu&4095U) || bytes!=QUSB_DOMAIN_PT_BYTES ||
     dma_bytes!=QUSB_DOMAIN_DMA_BYTES || !extent(pa,bytes,bits) || !extent(dma,dma_bytes,bits) ||
     (pa<dma+dma_bytes && dma<pa+bytes)) return QusbDomainBadArgument;
  zero(cpu,bytes);
  p[(QUSB_DOMAIN_IOVA>>30)&511U]=(pa+4096)|3U;
  p[512+((QUSB_DOMAIN_IOVA>>21)&511U)]=(pa+8192)|3U;
  for(i=0;i<8;i++) p[1024+i]=(dma+4096ULL*i)|LEAF_FLAGS;
  return QusbDomainOkay;
}

static int platform_ok(const QUSB_DOMAIN_PLATFORM *p)
{
  return p&&p->read32&&p->write32&&p->read64&&p->write64&&p->barrier&&p->delay_us&&p->clean_tables;
}
static QUSB_U64 reg_address(const QUSB_DOMAIN_LEASE *l,QUSB_U32 i)
{
  static const QUSB_U32 offset[]={0,0,0x10,0x30,0x20,0x28,0x38,0x3c,0x34,0};
  if(i==0) return l->gr1_base+0x800+4*l->cb;
  if(i==1) return l->gr1_base+4*l->cb;
  return l->cb_base+offset[i];
}
static QUSB_U64 reg_value(const QUSB_DOMAIN_LEASE *l,QUSB_U32 i)
{
  QUSB_U32 ps=l->id2&15U; /* S1 output IPA size; physical extents also obey OAS. */
  switch(i) {
    case 0:return 1; /* CBA2R.VA64; VMID0 for S1 domain, 16-bit ASID in TCR2. */
    case 1:return 0x1f300U; /* CBAR S1_TRANS/S2_BYPASS, WB, NSH per Linux. */
    case 2:return l->tcr2_preserved|0x38010U|ps; /* Preserve current noncritical controls. */
    case 3:return 0x803520U; /* TCR EPD1, SH_IS, ORGN/IRGN_WBWA, T0SZ32. */
    case 4:return l->table_pa|((QUSB_U64)l->asid<<48);
    case 5:return (QUSB_U64)l->asid<<48;
    case 6:return 0xffU; /* MAIR index0 normal WBWA. */
    case 7:case 8:return 0;
    case 9:return SCTLR_VALUE;
    default:return 0;
  }
}
static int lease_shape(const QUSB_DOMAIN_LEASE *l)
{
  QUSB_U32 i,shift,pages,bits1,bits2;
  if(!l||l->magic!=MAGIC||l->active!=1||!l->stream_count||l->stream_count>128||
     !l->bank_count||l->bank_count>128||l->index>=l->stream_count||l->cb>=l->bank_count||
     l->cb<((l->id1>>16)&255U)||
     l->extended_stream_ids>1||l->enable_attempted>1||l->tlb_released>1||l->tlb_poisoned>1||
     l->tcr2_bound>1||l->tcr2_view_owned>1||l->tcr2_capture_pending>1||
     (l->tcr2_preserved&TCR2_CRITICAL)||(l->tcr2_view_owned&&!l->tcr2_bound)||
     l->asid<256||l->asid>65535||l->iova!=QUSB_DOMAIN_IOVA||
     (l->original_cr0&0x409U)!=(0x400U|(l->extended_stream_ids<<3))) return 0;
  bits1=physical_bits(l->id2&15U);bits2=physical_bits((l->id2>>4)&15U);
  if(!bits1||!bits2||!(l->id0&(1U<<30))||!(l->id0&(1U<<27))||!(l->id2&(1U<<12))||
     !extent(l->table_pa,QUSB_DOMAIN_PT_BYTES,bits1<bits2?bits1:bits2)||
     !extent(l->dma_pa,QUSB_DOMAIN_DMA_BYTES,bits1<bits2?bits1:bits2)||
     (l->table_pa<l->dma_pa+QUSB_DOMAIN_DMA_BYTES&&l->dma_pa<l->table_pa+QUSB_DOMAIN_PT_BYTES))return 0;
  shift=(l->id1>>31)?16U:12U;pages=2U<<((l->id1>>28)&7U);
  if(l->gr1_base!=QUSB_SMMU_BASE+(1ULL<<shift)||
     l->cb_base!=QUSB_SMMU_BASE+(((QUSB_U64)pages+l->cb)<<shift)||
     l->cb_base+0x8f4U>QUSB_SMMU_BASE+QUSB_SMMU_BYTES||
     l->new_smr!=(l->extended_stream_ids?QUSB_STREAM_ID:(0x80000000U | QUSB_STREAM_ID))||
     l->new_s2cr!=(l->cb|(l->extended_stream_ids<<10))||
     (l->extended_stream_ids?(l->old_s2cr&(1U<<10)):(l->old_smr&0x80000000U))) return 0;
  for(i=0;i<QUSB_DOMAIN_REGISTER_COUNT;i++) {
    if(l->regs[i].address!=reg_address(l,i)||l->regs[i].installed!=reg_value(l,i)||
       l->regs[i].width!=((i==4||i==5)?8U:4U)||l->regs[i].touched>1U) return 0;
  }
  if(l->tcr2_bound&&
     ((l->tcr2_format_cba2r!=l->regs[0].saved&&l->tcr2_format_cba2r!=l->regs[0].installed)||
      (l->tcr2_format_cbar!=l->regs[1].saved&&l->tcr2_format_cbar!=l->regs[1].installed)))return 0;
  return !(l->regs[9].saved&1U);
}
int QcomUsbDomainWriteAllowed(const QUSB_DOMAIN_LEASE *l,QUSB_U64 a,QUSB_U64 v,QUSB_U32 width)
{
  QUSB_U64 smr,s2cr;
  if(!lease_shape(l)) return 0;
  smr=QUSB_SMMU_BASE+0x800+4*l->index;s2cr=QUSB_SMMU_BASE+0xc00+4*l->index;
  if(l->phase==CFG||l->phase==RESTORE_CFG) {
    if(l->operation>=10||(l->phase==CFG&&l->operation>=9)) return 0;
    if(l->phase==CFG&&l->operation==2&&
       (!l->tcr2_bound||!l->tcr2_view_owned||
        l->tcr2_format_cba2r!=l->regs[0].installed||l->tcr2_format_cbar!=l->regs[1].installed||
        l->tcr2_preserved!=(l->tcr2_observed&~TCR2_CRITICAL)))return 0;
    return a==l->regs[l->operation].address&&width==l->regs[l->operation].width&&
      v==(l->phase==CFG?l->regs[l->operation].installed:l->regs[l->operation].saved);
  }
  if(width!=4) return 0;
  switch(l->phase) {
    case TLBI:return a==l->cb_base+0x610&&v==l->asid;
    case SYNC:return a==l->cb_base+0x7f0&&v==0xffffffffU;
    case ENABLE:return a==l->cb_base&&v==SCTLR_VALUE;
    case DISABLE:return a==l->cb_base&&v==0;
    case S2PREP:return a==s2cr&&v==l->cb;
    case SMRPREP:return a==smr&&v==QUSB_STREAM_ID;
    case COMMIT:return l->extended_stream_ids?a==s2cr&&v==l->new_s2cr:a==smr&&v==l->new_smr;
    case INVALIDATE:return l->extended_stream_ids?a==s2cr&&v==l->cb:a==smr&&v==QUSB_STREAM_ID;
    case PAIR_FIRST:return l->extended_stream_ids?a==smr&&v==l->old_smr:a==s2cr&&v==l->old_s2cr;
    case PAIR_LAST:return l->extended_stream_ids?a==s2cr&&v==l->old_s2cr:a==smr&&v==l->old_smr;
    default:return 0;
  }
}
static int read_reg(const QUSB_DOMAIN_PLATFORM *p,QUSB_U64 a,QUSB_U32 width,QUSB_U64 *v)
{
  QUSB_U32 small;
  if(width==8) return p->read64(p->cookie,a,v);
  if(!p->read32(p->cookie,a,&small)) return 0;
  *v=small;return 1;
}
static QUSB_DOMAIN_RESULT mode_ok(const QUSB_DOMAIN_PLATFORM *p,const QUSB_DOMAIN_LEASE *l)
{
  QUSB_U32 v,cr2;
  if(!p->read32(p->cookie,QUSB_SMMU_BASE,&v)||!p->read32(p->cookie,QUSB_SMMU_BASE+8,&cr2))
    return QusbDomainReadFailed;
  return (v&0x409U)==(l->original_cr0&0x409U)&&!(cr2&0xa0000000U)?
    QusbDomainOkay:QusbDomainOwnershipChanged;
}
/* A format write may change read-only/reserved or inherited functional bits in
 * the TCR2 view. Capture that exact view immediately; never mask later checks.
 * Pending captures are retried only while the selected bank is disabled.
 */
static QUSB_DOMAIN_RESULT capture_tcr2(const QUSB_DOMAIN_PLATFORM *p,QUSB_DOMAIN_LEASE *l)
{
  QUSB_U32 a,b,s,v;
  if(!p->read32(p->cookie,l->regs[0].address,&a)||
     !p->read32(p->cookie,l->regs[1].address,&b)||
     !p->read32(p->cookie,l->cb_base,&s)||
     !p->read32(p->cookie,l->regs[2].address,&v))return QusbDomainReadFailed;
  if((s&1U)||(a!=l->regs[0].saved&&a!=l->regs[0].installed)||
     (b!=l->regs[1].saved&&b!=l->regs[1].installed))return QusbDomainOwnershipChanged;
  l->tcr2_observed=v;l->tcr2_format_cba2r=a;l->tcr2_format_cbar=b;
  l->tcr2_bound=1;l->tcr2_view_owned=1;l->tcr2_capture_pending=0;
  return QusbDomainOkay;
}
static QUSB_DOMAIN_RESULT halted(const QUSB_DOMAIN_PLATFORM *p)
{
  QUSB_U32 g,c,s;
  if(!p->read32(p->cookie,QUSB_DWC3_BASE+0xc110,&g)||
     !p->read32(p->cookie,QUSB_DWC3_BASE+0xc704,&c)||
     !p->read32(p->cookie,QUSB_DWC3_BASE+0xc70c,&s)) return QusbDomainReadFailed;
  return ((g>>12)&3U)==2U&&!(c&(1U<<31))&&(s&(1U<<22))?QusbDomainOkay:QusbDomainCoreRunning;
}
static QUSB_DOMAIN_RESULT write_value(const QUSB_DOMAIN_PLATFORM *p,QUSB_DOMAIN_LEASE *l,
                                     QUSB_U32 phase,QUSB_U32 op,QUSB_U64 a,QUSB_U64 v,QUSB_U32 width,int check)
{
  QUSB_DOMAIN_RESULT r;int written;
  l->phase=phase;l->operation=op;l->stage=phase;l->last_address=a;l->last_expected=v;
  if(!QcomUsbDomainWriteAllowed(l,a,v,width)) return QusbDomainBadArgument;
  r=mode_ok(p,l);if(r) return r;
  p->barrier(p->cookie);
  written=width==8?p->write64(p->cookie,a,v):p->write32(p->cookie,a,(QUSB_U32)v);
  p->barrier(p->cookie);
  if(!written) return QusbDomainWriteFailed;
  if(check) {
    if(!read_reg(p,a,width,&l->last_readback)) return QusbDomainReadFailed;
    if(l->last_readback!=v) return QusbDomainWriteFailed;
  }
  return QusbDomainOkay;
}
static QUSB_DOMAIN_RESULT tlb_sync(const QUSB_DOMAIN_PLATFORM *p,QUSB_DOMAIN_LEASE *l)
{
  QUSB_DOMAIN_RESULT r;QUSB_U32 n,v;
  if(l->tlb_poisoned)return QusbDomainTlbTimeout;
  /* A failed/timeout maintenance request must never be queued again: the
   * QSMMUv500 driver warns a non-draining TBU queue can stall later stores. */
  l->tlb_poisoned=1;
  r=write_value(p,l,TLBI,0,l->cb_base+0x610,l->asid,4,0);if(r)return r;
  r=write_value(p,l,SYNC,0,l->cb_base+0x7f0,0xffffffffU,4,0);if(r)return r;
  for(n=0;n<1000;n++) {
    if(!p->read32(p->cookie,l->cb_base+0x7f4,&v))return QusbDomainReadFailed;
    if(!(v&1U)){l->tlb_poisoned=0;return QusbDomainOkay;}
    p->delay_us(p->cookie,1);
  }
  return QusbDomainTlbTimeout;
}

/* Scans the complete supported NS table. No other stream may cover SID40. */
static QUSB_DOMAIN_RESULT scan(const QUSB_DOMAIN_PLATFORM *p,QUSB_DOMAIN_LEASE *l,
                               unsigned char used[128],int installed)
{
  QUSB_U32 i,smr,s2cr,mask,id,matches=0,first=0xffffffffU,oldsmr=0,olds2cr=0;
  for(i=0;i<l->stream_count;i++) {
    if(!p->read32(p->cookie,QUSB_SMMU_BASE+0x800+4*i,&smr)||
       !p->read32(p->cookie,QUSB_SMMU_BASE+0xc00+4*i,&s2cr))return QusbDomainReadFailed;
    if(l->extended_stream_ids?!(s2cr&(1U<<10)):!(smr&0x80000000U)) {
      if(first==0xffffffffU) {first=i;oldsmr=smr;olds2cr=s2cr;} continue;
    }
    if(used&&(s2cr&255U)<l->bank_count) used[s2cr&255U]=1;
    mask=(smr>>16)&(l->extended_stream_ids?0xffffU:0x7fffU);
    id=smr&(l->extended_stream_ids?0xffffU:0x7fffU);
    if((QUSB_STREAM_ID&~mask)!=(id&~mask))continue;
    matches++;
    if(!installed||i!=l->index||smr!=l->new_smr||s2cr!=l->new_s2cr) {
      /* Report the conflicting slot and pair instead of a zeroed lease. */
      if(!installed&&matches==1){l->index=i;l->old_smr=smr;l->old_s2cr=s2cr;}
      return QusbDomainStreamExists;
    }
  }
  if(installed)return matches==1?QusbDomainOkay:QusbDomainOwnershipChanged;
  if(matches)return QusbDomainStreamExists;
  if(first==0xffffffffU)return QusbDomainNoSlot;
  l->index=first;l->old_smr=oldsmr;l->old_s2cr=olds2cr;return QusbDomainOkay;
}

QUSB_DOMAIN_RESULT QcomUsbDomainInstall(const QUSB_DOMAIN_PLATFORM *p,void *cpu,QUSB_U64 pt,
                                       QUSB_U32 pt_bytes,QUSB_U64 dma,QUSB_U32 dma_bytes,
                                       QUSB_DOMAIN_LEASE *l)
{
  QUSB_U32 i,j,v,cr2,shift,pages,ns2,chosen=0xffffffffU,bits,ps;
  QUSB_U64 tt0,tt1,used_asid[256],value;unsigned char used[128];
  QUSB_DOMAIN_RESULT r;
  if(!platform_ok(p)||!l||l->active)return QusbDomainBadArgument;
  zero(l,sizeof(*l));zero(used,sizeof(used));zero(used_asid,sizeof(used_asid));
  r=halted(p);if(r)return r;
  if(!p->read32(p->cookie,QUSB_SMMU_BASE+0x20,&l->id0)||
     !p->read32(p->cookie,QUSB_SMMU_BASE+0x24,&l->id1)||
     !p->read32(p->cookie,QUSB_SMMU_BASE+0x28,&l->id2)||
     !p->read32(p->cookie,QUSB_SMMU_BASE,&l->original_cr0)||
     !p->read32(p->cookie,QUSB_SMMU_BASE+8,&cr2))return QusbDomainReadFailed;
  l->stream_count=l->id0&255U;l->bank_count=l->id1&255U;
  l->extended_stream_ids=(l->original_cr0>>3)&1U;
  if(!l->stream_count||l->stream_count>128||!l->bank_count||l->bank_count>128||
     !(l->id0&(1U<<30))||!(l->id0&(1U<<27))||!(l->id2&(1U<<12))||
     (l->original_cr0&0x401U)!=0x400U||(cr2&0xa0000000U)||
     (l->extended_stream_ids&&!(l->id0&(1U<<8))))return QusbDomainUnsupported;
  ps=(l->id2&15U)<((l->id2>>4)&15U)?(l->id2&15U):((l->id2>>4)&15U);
  bits=physical_bits(ps);if(!bits)return QusbDomainUnsupported;
  r=QcomUsbDomainBuildTables(cpu,pt,pt_bytes,dma,dma_bytes,bits);if(r)return r;
  shift=(l->id1>>31)?16U:12U;pages=2U<<((l->id1>>28)&7U);
  l->gr1_base=QUSB_SMMU_BASE+(1ULL<<shift);
  if(QUSB_SMMU_BASE+(((QUSB_U64)pages+l->bank_count)<<shift)>QUSB_SMMU_BASE+QUSB_SMMU_BYTES)
    return QusbDomainUnsupported;
  ns2=(l->id1>>16)&255U;if(ns2>=l->bank_count)return QusbDomainNoBank;
  r=scan(p,l,used,0);if(r)return r;
  for(i=0;i<l->bank_count;i++) {
    if(!p->read32(p->cookie,l->gr1_base+4*i,&v))return QusbDomainReadFailed;
    if(((v>>16)&3U)==3U&&((v>>8)&255U)<l->bank_count)used[(v>>8)&255U]=1;
    /* Stage2 banks have VMIDs, not S1 ASIDs, and no TTBR1 register. */
    if(((v>>16)&3U)==0U)continue;
    value=QUSB_SMMU_BASE+(((QUSB_U64)pages+i)<<shift);
    if(!p->read64(p->cookie,value+0x20,&tt0)||!p->read64(p->cookie,value+0x28,&tt1))return QusbDomainReadFailed;
    used_asid[2*i]=tt0>>48;used_asid[2*i+1]=tt1>>48;
  }
  for(i=ns2;i<l->bank_count;i++) {
    value=QUSB_SMMU_BASE+(((QUSB_U64)pages+i)<<shift);
    if(used[i])continue;
    if(!p->read32(p->cookie,value,&v))return QusbDomainReadFailed;
    if(v&1U)continue;
    if(!p->read32(p->cookie,value+0x58,&v))return QusbDomainReadFailed;
    if(fsr_idle(v)){chosen=i;break;}
  }
  if(chosen==0xffffffffU)return QusbDomainNoBank;
  l->cb=chosen;l->cb_base=QUSB_SMMU_BASE+(((QUSB_U64)pages+chosen)<<shift);
  for(i=0xff00U;i>0xfd00U;i--) {
    for(j=0;j<2*l->bank_count;j++)if(used_asid[j]==i)break;
    if(j==2*l->bank_count){l->asid=i;break;}
  }
  if(!l->asid)return QusbDomainNoAsid;
  l->table_pa=pt;l->dma_pa=dma;l->iova=QUSB_DOMAIN_IOVA;
  l->new_smr=l->extended_stream_ids?QUSB_STREAM_ID:(0x80000000U | QUSB_STREAM_ID);
  l->new_s2cr=l->cb|(l->extended_stream_ids<<10);
  for(i=0;i<QUSB_DOMAIN_REGISTER_COUNT;i++) {
    l->regs[i].address=reg_address(l,i);l->regs[i].width=(i==4||i==5)?8:4;
    l->regs[i].installed=reg_value(l,i);
    if(!read_reg(p,l->regs[i].address,l->regs[i].width,&l->regs[i].saved))return QusbDomainReadFailed;
  }
  l->tcr2_preserved=(QUSB_U32)l->regs[2].saved&~TCR2_CRITICAL;
  l->regs[2].installed=reg_value(l,2);
  if(l->regs[9].saved&1U)return QusbDomainOwnershipChanged;
  if(!p->read32(p->cookie,l->cb_base+4,&l->old_actlr))return QusbDomainReadFailed;
  if(!p->clean_tables(p->cookie,cpu,pt_bytes))return QusbDomainCleanFailed;
  p->barrier(p->cookie);
  r=mode_ok(p,l);if(r)return r;
  /* Re-read the selected inactive bank and pair before the first MMIO write. */
  for(i=0;i<QUSB_DOMAIN_REGISTER_COUNT;i++) {
    if(!read_reg(p,l->regs[i].address,l->regs[i].width,&value))return QusbDomainReadFailed;
    if(value!=l->regs[i].saved)return QusbDomainOwnershipChanged;
  }
  if(!p->read32(p->cookie,QUSB_SMMU_BASE+0x800+4*l->index,&v)||v!=l->old_smr||
     !p->read32(p->cookie,QUSB_SMMU_BASE+0xc00+4*l->index,&v)||v!=l->old_s2cr)
    return QusbDomainOwnershipChanged;
  l->magic=MAGIC;l->active=1;
  for(i=0;i<9;i++) {
    if(i==2) {
      r=capture_tcr2(p,l);if(r)return r;
      l->tcr2_preserved=l->tcr2_observed&~TCR2_CRITICAL;
      l->regs[2].installed=reg_value(l,2);
    }
    if(i<2)l->tcr2_capture_pending=1;
    l->regs[i].touched=1;
    r=write_value(p,l,CFG,i,l->regs[i].address,l->regs[i].installed,l->regs[i].width,1);
    if(i<2) {
      QUSB_DOMAIN_RESULT captured=capture_tcr2(p,l);
      if(r)return r;
      if(captured)return captured;
    } else if(r)return r;
    if(i==2)l->tcr2_view_owned=0;
  }
  r=tlb_sync(p,l);if(r)return r;
  l->enable_attempted=1;l->regs[9].touched=1;
  r=write_value(p,l,ENABLE,9,l->cb_base,SCTLR_VALUE,4,1);if(r)return r;
  r=write_value(p,l,S2PREP,0,QUSB_SMMU_BASE+0xc00+4*l->index,l->cb,4,1);if(r)return r;
  r=write_value(p,l,SMRPREP,0,QUSB_SMMU_BASE+0x800+4*l->index,QUSB_STREAM_ID,4,1);if(r)return r;
  {
    QUSB_DOMAIN_LEASE checked=*l;
    r=scan(p,&checked,0,0);if(r)return r;
    if(checked.index!=l->index)return QusbDomainOwnershipChanged;
    if(!p->read32(p->cookie,QUSB_SMMU_BASE+0x800+4*l->index,&v)||v!=QUSB_STREAM_ID||
       !p->read32(p->cookie,QUSB_SMMU_BASE+0xc00+4*l->index,&v)||v!=l->cb)
      return QusbDomainOwnershipChanged;
    for(i=0;i<QUSB_DOMAIN_REGISTER_COUNT;i++) {
      if(!read_reg(p,l->regs[i].address,l->regs[i].width,&value))return QusbDomainReadFailed;
      if(value!=l->regs[i].installed)return QusbDomainOwnershipChanged;
    }
    if(!p->read32(p->cookie,l->cb_base+0x58,&v))return QusbDomainReadFailed;
    if(!fsr_a64(v))return QusbDomainOwnershipChanged;
  }
  r=write_value(p,l,COMMIT,0,QUSB_SMMU_BASE+(l->extended_stream_ids?0xc00:0x800)+4*l->index,
                l->extended_stream_ids?l->new_s2cr:l->new_smr,4,1);if(r)return r;
  r=scan(p,l,0,1);if(r)return r;
  for(i=0;i<QUSB_DOMAIN_REGISTER_COUNT;i++) {
    if(!read_reg(p,l->regs[i].address,l->regs[i].width,&value))return QusbDomainReadFailed;
    if(value!=l->regs[i].installed)return QusbDomainOwnershipChanged;
  }
  if(!p->read32(p->cookie,l->cb_base+0x58,&v))return QusbDomainReadFailed;
  if(!fsr_a64(v))return QusbDomainOwnershipChanged;
  l->phase=LIVE;l->stage=LIVE;return QusbDomainOkay;
}

QUSB_DOMAIN_RESULT QcomUsbDomainRestore(const QUSB_DOMAIN_PLATFORM *p,QUSB_DOMAIN_LEASE *l)
{
  QUSB_DOMAIN_RESULT r;QUSB_U32 i,smr,s2cr,v;QUSB_U64 value;
  if(!platform_ok(p)||!l)return QusbDomainBadArgument;
  if(!l->active)return QusbDomainOkay;
  if(!lease_shape(l))return QusbDomainBadArgument;
  if(l->tlb_poisoned)return QusbDomainTlbTimeout;
  r=halted(p);if(r)return r;r=mode_ok(p,l);if(r)return r;
  if(!p->read32(p->cookie,l->cb_base+0x58,&v))return QusbDomainReadFailed;
  if(!fsr_idle(v)||(l->phase==LIVE&&!fsr_a64(v)))
    return QusbDomainOwnershipChanged; /* Preserve actual fault/unknown evidence. */
  if(l->tcr2_capture_pending) {
    r=capture_tcr2(p,l);if(r)return r;
  }
  if(!p->read32(p->cookie,QUSB_SMMU_BASE+0x800+4*l->index,&smr)||
     !p->read32(p->cookie,QUSB_SMMU_BASE+0xc00+4*l->index,&s2cr)||
     !p->read32(p->cookie,l->cb_base+4,&v))return QusbDomainReadFailed;
  if(v!=l->old_actlr)return QusbDomainOwnershipChanged;
  if((smr!=l->old_smr&&smr!=QUSB_STREAM_ID&&smr!=l->new_smr)||
     (s2cr!=l->old_s2cr&&s2cr!=l->cb&&s2cr!=l->new_s2cr))return QusbDomainOwnershipChanged;
  for(i=0;i<QUSB_DOMAIN_REGISTER_COUNT;i++) {
    if(!read_reg(p,l->regs[i].address,l->regs[i].width,&value))return QusbDomainReadFailed;
    if(l->phase==LIVE&&value!=l->regs[i].installed)return QusbDomainOwnershipChanged;
    /* Before our ASID flush completes, no format register has legitimately
     * been restored. Retrying an invalidate/disable failure cannot accept
     * old ASID-width/TCR words and flush under the wrong format.
     */
    if(l->enable_attempted&&!l->tlb_released&&i<9&&value!=l->regs[i].installed)
      return QusbDomainOwnershipChanged;
    if(i==2&&l->tcr2_view_owned&&value==l->tcr2_observed) {
      QUSB_U32 a,b;
      if(!p->read32(p->cookie,l->regs[0].address,&a)||!p->read32(p->cookie,l->regs[1].address,&b))
        return QusbDomainReadFailed;
      if(a!=l->tcr2_format_cba2r||b!=l->tcr2_format_cbar)return QusbDomainOwnershipChanged;
      continue;
    }
    if(value!=l->regs[i].saved&&(!l->regs[i].touched||
       (value!=l->regs[i].installed&&!(i==9&&value==0))))return QusbDomainOwnershipChanged;
  }
  if(l->extended_stream_ids?(s2cr&(1U<<10)):(smr&0x80000000U)) {
    if(smr!=l->new_smr||s2cr!=l->new_s2cr)return QusbDomainOwnershipChanged;
    r=write_value(p,l,INVALIDATE,0,QUSB_SMMU_BASE+(l->extended_stream_ids?0xc00:0x800)+4*l->index,
                  l->extended_stream_ids?l->cb:QUSB_STREAM_ID,4,1);if(r)return r;
  }
  /* Flush before restoring address-format registers. Once released, retries
   * must not issue a 16-bit ASID invalidation under an old 8-bit format. A bank
   * never enabled cannot have used our tables/ASID and needs no such flush. */
  if(!l->tlb_released) {
    if(l->enable_attempted) {
      l->regs[9].touched=1;r=write_value(p,l,DISABLE,9,l->cb_base,0,4,1);if(r)return r;
      r=tlb_sync(p,l);if(r)return r;
    }
    l->tlb_released=1;
  }
  if(!p->read32(p->cookie,l->cb_base+0x58,&v))return QusbDomainReadFailed;
  if(!fsr_idle(v))return QusbDomainOwnershipChanged;
  for(i=0;i<9;i++) {
    if(i<2)l->tcr2_capture_pending=1;
    r=write_value(p,l,RESTORE_CFG,i,l->regs[i].address,l->regs[i].saved,l->regs[i].width,1);
    if(i<2) {
      QUSB_DOMAIN_RESULT captured=capture_tcr2(p,l);
      if(r)return r;
      if(captured)return captured;
    } else if(r)return r;
    if(i==2)l->tcr2_view_owned=0;
    l->regs[i].touched=0;
  }
  /* Original SCTLR.M0 is restored last after its original format and TTBRs. */
  r=write_value(p,l,RESTORE_CFG,9,l->cb_base,l->regs[9].saved,4,1);if(r)return r;
  l->regs[9].touched=0;
  r=write_value(p,l,PAIR_FIRST,0,QUSB_SMMU_BASE+(l->extended_stream_ids?0x800:0xc00)+4*l->index,
                l->extended_stream_ids?l->old_smr:l->old_s2cr,4,1);if(r)return r;
  r=write_value(p,l,PAIR_LAST,0,QUSB_SMMU_BASE+(l->extended_stream_ids?0xc00:0x800)+4*l->index,
                l->extended_stream_ids?l->old_s2cr:l->old_smr,4,1);if(r)return r;
  l->active=0;return QusbDomainOkay;
}
const char *QcomUsbDomainResultName(QUSB_DOMAIN_RESULT r)
{
  static const char *const names[]={"okay","bad argument","read failed","unsupported SMMU",
    "USB not halted in DEVICE role","USB stream exists","no free stream slot","no free NS context bank",
    "no unused ASID","ownership changed","write failed","local TLB timeout","table cache clean failed"};
  return (QUSB_U32)r<sizeof(names)/sizeof(names[0])?names[r]:"unknown";
}
