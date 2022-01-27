#include "mold.h"

#include <tbb/parallel_for.h>
#include <tbb/parallel_for_each.h>

namespace mold::elf {

using E = RISCV64;

static u32 bit(u32 val, i64 pos) {
  return (val & (1 << pos)) ? 1 : 0;
}

// Returns [hi:lo] bits of val.
static u32 bits(u32 val, i64 hi, i64 lo) {
  return (val >> lo) & ((1LL << (hi - lo + 1)) - 1);
}

static u32 itype(u32 val) {
  return val << 20;
}

static u32 stype(u32 val) {
  return bits(val, 11, 5) << 25 | bits(val, 4, 0) << 7;
}

static u32 btype(u32 val) {
  return bit(val, 12) << 31 | bits(val, 10, 5) << 25 |
         bits(val, 4, 1) << 8 | bit(val, 11) << 7;
}

static u32 utype(u32 val) {
  // U-type instructions are used in combination with I-type
  // instructions. U-type insn sets an immediate to the upper 20-bits
  // of a register. I-type insn sign-extends a 12-bits immediate and
  // add it to a register value to construct a complete value. 0x800
  // is added here to compensate for the sign-extension.
  return bits(val + 0x800, 31, 12) << 12;
}

static u32 jtype(u32 val) {
  return bit(val, 20) << 31 | bits(val, 10, 1) << 21 |
         bit(val, 11) << 20 | bits(val, 19, 12) << 12;
}

static u32 cbtype(u32 val) {
  return bit(val, 8) << 12 | bit(val, 4) << 11 | bit(val, 3) << 10 |
         bit(val, 7) << 6  | bit(val, 6) << 5  | bit(val, 2) << 4  |
         bit(val, 1) << 3  | bit(val, 5) << 2;
}

static u32 cjtype(u32 val) {
  return bit(val, 11) << 12 | bit(val, 4)  << 11 | bit(val, 9) << 10 |
         bit(val, 8)  << 9  | bit(val, 10) << 8  | bit(val, 6) << 7  |
         bit(val, 7)  << 6  | bit(val, 3)  << 5  | bit(val, 2) << 4  |
         bit(val, 1)  << 3  | bit(val, 5)  << 2;
}

static void write_itype(u32 *loc, u32 val) {
  u32 mask = 0b000000'00000'11111'111'11111'1111111;
  *loc = (*loc & mask) | itype(val);
}

static void write_stype(u32 *loc, u32 val) {
  u32 mask = 0b000000'11111'11111'111'00000'1111111;
  *loc = (*loc & mask) | stype(val);
}

static void write_btype(u32 *loc, u32 val) {
  u32 mask = 0b000000'11111'11111'111'00000'1111111;
  *loc = (*loc & mask) | btype(val);
}

static void write_utype(u32 *loc, u32 val) {
  u32 mask = 0b000000'00000'00000'000'11111'1111111;
  *loc = (*loc & mask) | utype(val);
}

static void write_jtype(u32 *loc, u32 val) {
  u32 mask = 0b000000'00000'00000'000'11111'1111111;
  *loc = (*loc & mask) | jtype(val);
}

static void write_cbtype(u16 *loc, u32 val) {
  u32 mask = 0b1110001110000011;
  *loc = (*loc & mask) | cbtype(val);
}

static void write_cjtype(u16 *loc, u32 val) {
  u32 mask = 0b1110000000000011;
  *loc = (*loc & mask) | cjtype(val);
}

static void write_plt_header(Context<E> &ctx) {
  u32 *buf = (u32 *)(ctx.buf + ctx.plt->shdr.sh_offset);

  static const u32 plt0[] = {
    0x00000397, // auipc  t2, %pcrel_hi(.got.plt)
    0x41c30333, // sub    t1, t1, t3               # .plt entry + hdr + 12
    0x0003be03, // ld     t3, %pcrel_lo(1b)(t2)    # _dl_runtime_resolve
    0xfd430313, // addi   t1, t1, -44              # .plt entry
    0x00038293, // addi   t0, t2, %pcrel_lo(1b)    # &.got.plt
    0x00135313, // srli   t1, t1, 1                # .plt entry offset
    0x0082b283, // ld     t0, 8(t0)                # link map
    0x000e0067, // jr     t3
  };

  u64 gotplt = ctx.gotplt->shdr.sh_addr;
  u64 plt = ctx.plt->shdr.sh_addr;

  memcpy(buf, plt0, sizeof(plt0));
  write_utype(buf, gotplt - plt);
  write_itype(buf + 2, gotplt - plt);
  write_itype(buf + 4, gotplt - plt);
}

static void write_plt_entry(Context<E> &ctx, Symbol<E> &sym) {
  u32 *ent = (u32 *)(ctx.buf + ctx.plt->shdr.sh_offset + ctx.plt_hdr_size +
                     sym.get_plt_idx(ctx) * ctx.plt_size);

  static const u32 data[] = {
    0x00000e17, // auipc   t3, %pcrel_hi(function@.got.plt)
    0x000e3e03, // ld      t3, %pcrel_lo(1b)(t3)
    0x000e0367, // jalr    t1, t3
    0x00000013, // nop
  };

  u64 gotplt = sym.get_gotplt_addr(ctx);
  u64 plt = sym.get_plt_addr(ctx);

  memcpy(ent, data, sizeof(data));
  write_utype(ent, gotplt - plt);
  write_itype(ent + 1, gotplt - plt);
}

template <>
void PltSection<E>::copy_buf(Context<E> &ctx) {
  write_plt_header(ctx);
  for (Symbol<E> *sym : symbols)
    write_plt_entry(ctx, *sym);
}

template <>
void PltGotSection<E>::copy_buf(Context<E> &ctx) {
  u32 *buf = (u32 *)(ctx.buf + ctx.plt->shdr.sh_offset);

  static const u32 data[] = {
    0x00000e17, // auipc   t3, %pcrel_hi(function@.got.plt)
    0x000e3e03, // ld      t3, %pcrel_lo(1b)(t3)
    0x000e0367, // jalr    t1, t3
    0x00000013, // nop
  };

  for (Symbol<E> *sym : symbols) {
    u32 *ent = buf + sym->get_pltgot_idx(ctx) * 4;
    u64 got = sym->get_got_addr(ctx);
    u64 plt = sym->get_plt_addr(ctx);

    memcpy(ent, data, sizeof(data));
    write_utype(ent, got - plt);
    write_itype(ent + 1, got - plt);
  }
}

template <>
void EhFrameSection<E>::apply_reloc(Context<E> &ctx, ElfRel<E> &rel,
                                    u64 offset, u64 val) {
  u8 *loc = ctx.buf + this->shdr.sh_offset + offset;

  switch (rel.r_type) {
  case R_RISCV_ADD32:
    *(u32 *)loc += val;
    return;
  case R_RISCV_SUB8:
    *loc -= val;
    return;
  case R_RISCV_SUB16:
    *(u16 *)loc -= val;
    return;
  case R_RISCV_SUB32:
    *(u32 *)loc -= val;
    return;
  case R_RISCV_SUB6:
    *loc = (*loc - val) & 0b11'1111;
    return;
  case R_RISCV_SET6:
    *loc = (*loc + val) & 0b11'1111;
    return;
  case R_RISCV_SET8:
    *loc = val;
    return;
  case R_RISCV_SET16:
    *(u16 *)loc = val;
    return;
  case R_RISCV_32_PCREL:
    *(u32 *)loc = val - this->shdr.sh_addr - offset;
    return;
  }
  Fatal(ctx) << "unsupported relocation in .eh_frame: " << rel;
}

template <>
void InputSection<E>::apply_reloc_alloc(Context<E> &ctx, u8 *base) {
  ElfRel<E> *dynrel = nullptr;
  std::span<ElfRel<E>> rels = get_rels(ctx);

  i64 frag_idx = 0;

  if (ctx.reldyn)
    dynrel = (ElfRel<E> *)(ctx.buf + ctx.reldyn->shdr.sh_offset +
                           file.reldyn_offset + this->reldyn_offset);

  for (i64 i = 0; i < rels.size(); i++) {
    const ElfRel<E> &rel = rels[i];
    if (rel.r_type == R_RISCV_NONE)
      continue;

    Symbol<E> &sym = *file.symbols[rel.r_sym];
    u8 *loc = base + rel.r_offset;

    const SectionFragmentRef<E> *frag_ref = nullptr;
    if (rel_fragments && rel_fragments[frag_idx].idx == i)
      frag_ref = &rel_fragments[frag_idx++];

    auto overflow_check = [&](i64 val, i64 lo, i64 hi) {
      if (val < lo || hi <= val)
        Error(ctx) << *this << ": relocation " << rel << " against "
                   << sym << " out of range: " << val << " is not in ["
                   << lo << ", " << hi << ")";
    };

#define S   (frag_ref ? frag_ref->frag->get_addr(ctx) : sym.get_addr(ctx))
#define A   (frag_ref ? frag_ref->addend : rel.r_addend)
#define P   (output_section->shdr.sh_addr + offset + rel.r_offset)
#define G   (sym.get_got_addr(ctx) - ctx.got->shdr.sh_addr)
#define GOT ctx.got->shdr.sh_addr

    if (needs_dynrel[i]) {
      *dynrel++ = {P, R_RISCV_64, (u32)sym.get_dynsym_idx(ctx), A};
      *(u64 *)loc = A;
      continue;
    }

    if (needs_baserel[i]) {
      if (!is_relr_reloc(ctx, rel))
        *dynrel++ = {P, R_RISCV_RELATIVE, 0, (i64)(S + A)};
      *(u64 *)loc = S + A;
      continue;
    }

    switch (rel.r_type) {
    case R_RISCV_32:
      *(u32 *)loc = S + A;
      break;
    case R_RISCV_64:
      *(u64 *)loc = S + A;
      break;
    case R_RISCV_TLS_DTPMOD32:
    case R_RISCV_TLS_DTPMOD64:
    case R_RISCV_TLS_DTPREL32:
    case R_RISCV_TLS_DTPREL64:
    case R_RISCV_TLS_TPREL32:
    case R_RISCV_TLS_TPREL64:
      Error(ctx) << *this << ": unsupported relocation: " << rel;
      break;
    case R_RISCV_BRANCH:
      write_btype((u32 *)loc, S + A - P);
      break;
    case R_RISCV_JAL:
      write_jtype((u32 *)loc, S + A - P);
      break;
    case R_RISCV_CALL:
    case R_RISCV_CALL_PLT: {
      u64 val = sym.esym().is_undef_weak() ? 0 : S + A - P;
      write_utype((u32 *)loc, val);
      write_itype((u32 *)(loc + 4), val); // errata
      break;
    }
    case R_RISCV_GOT_HI20:
      *(u32 *)loc = G + GOT + A - P; // errata
      break;
    case R_RISCV_TLS_GOT_HI20:
    case R_RISCV_TLS_GD_HI20:
      Error(ctx) << *this << ": unsupported relocation: " << rel;
      break;
    case R_RISCV_PCREL_HI20:
      if (sym.esym().is_undef_weak()) {
        // On RISC-V, calling an undefined weak symbol jumps to the same
        // instruction, which effectively hangs up the running program.
        // This should help debugging of a faulty program.
        *(u32 *)loc = P;
      } else {
        *(u32 *)loc = S + A - P;
      }
      break;
    case R_RISCV_PCREL_LO12_I:
      assert(sym.input_section == this);
      assert(sym.value < rel.r_offset);
      write_itype((u32 *)loc, *(u32 *)(base + sym.value));
      break;
    case R_RISCV_LO12_I:
    case R_RISCV_TPREL_LO12_I:
      write_itype((u32 *)loc, S + A);
      break;
    case R_RISCV_PCREL_LO12_S:
      assert(sym.input_section == this);
      assert(sym.value < rel.r_offset);
      write_stype((u32 *)loc, *(u32 *)(base + sym.value));
      break;
    case R_RISCV_LO12_S:
    case R_RISCV_TPREL_LO12_S:
      write_stype((u32 *)loc, S + A);
      break;
    case R_RISCV_HI20:
      write_utype((u32 *)loc, S + A);
      break;
    case R_RISCV_TPREL_HI20:
      write_utype((u32 *)loc, S + A - ctx.tls_begin);
      break;
    case R_RISCV_TPREL_ADD:
      break;
    case R_RISCV_ADD8:
      loc += S + A;
      break;
    case R_RISCV_ADD16:
      *(u16 *)loc += S + A;
      break;
    case R_RISCV_ADD32:
      *(u32 *)loc += S + A;
      break;
    case R_RISCV_ADD64:
      *(u64 *)loc += S + A;
      break;
    case R_RISCV_SUB8:
      loc -= S + A;
      break;
    case R_RISCV_SUB16:
      *(u16 *)loc -= S + A;
      break;
    case R_RISCV_SUB32:
      *(u32 *)loc -= S + A;
      break;
    case R_RISCV_SUB64:
      *(u64 *)loc -= S + A;
      break;
    case R_RISCV_ALIGN:
      break;
    case R_RISCV_RVC_BRANCH:
      write_cbtype((u16 *)loc, S + A - P);
      break;
    case R_RISCV_RVC_JUMP:
      write_cjtype((u16 *)loc, S + A - P);
      break;
    case R_RISCV_RVC_LUI:
      Error(ctx) << *this << ": unsupported relocation: " << rel;
      break;
    case R_RISCV_RELAX:
      break;
    case R_RISCV_SUB6:
    case R_RISCV_SET6:
    case R_RISCV_SET8:
    case R_RISCV_SET16:
    case R_RISCV_SET32:
    case R_RISCV_32_PCREL:
      Error(ctx) << *this << ": unsupported relocation: " << rel;
      break;
    default:
      Error(ctx) << *this << ": unknown relocation: " << rel;
    }

#undef S
#undef A
#undef P
#undef G
#undef GOT
  }

  // In the above loop, PC-relative HI20 relocations overwrote
  // instructions with full 32-bit values to allow their corresponding
  // PCREL_LO12 relocations to read their values. This loop restore
  // the original instructions.
  for (i64 i = 0; i < rels.size(); i++) {
    const ElfRel<E> &r = rels[i];
    u32 *loc = (u32 *)(base + r.r_offset);

    switch (r.r_type) {
    case R_RISCV_GOT_HI20:
    case R_RISCV_PCREL_HI20: {
      u32 val = *loc;
      *loc = *(u32 *)&contents[r.r_offset];
      write_utype(loc, val);
      break;
    }
    }
  }
}

template <>
void InputSection<E>::apply_reloc_nonalloc(Context<E> &ctx, u8 *base) {}

template <>
void InputSection<E>::scan_relocations(Context<E> &ctx) {
  assert(shdr.sh_flags & SHF_ALLOC);

  this->reldyn_offset = file.num_dynrel * sizeof(ElfRel<E>);
  std::span<ElfRel<E>> rels = get_rels(ctx);

  // Scan relocations
  for (i64 i = 0; i < rels.size(); i++) {
    const ElfRel<E> &rel = rels[i];
    if (rel.r_type == R_RISCV_NONE)
      continue;

    Symbol<E> &sym = *file.symbols[rel.r_sym];

    if (!sym.file) {
      report_undef(ctx, sym);
      continue;
    }

    if (sym.get_type() == STT_GNU_IFUNC) {
      sym.flags |= NEEDS_GOT;
      sym.flags |= NEEDS_PLT;
    }

    switch (rel.r_type) {
    case R_RISCV_32:
    case R_RISCV_HI20: {
      Action table[][4] = {
        // Absolute  Local    Imported data  Imported code
        {  NONE,     NONE,    ERROR,         ERROR },      // DSO
        {  NONE,     NONE,    COPYREL,       PLT   },      // PIE
        {  NONE,     NONE,    COPYREL,       PLT   },      // PDE
      };
      dispatch(ctx, table, i, rel, sym);
      break;
    }
    case R_RISCV_64: {
      Action table[][4] = {
        // Absolute  Local    Imported data  Imported code
        {  NONE,     BASEREL, DYNREL,        DYNREL },     // DSO
        {  NONE,     BASEREL, DYNREL,        DYNREL },     // PIE
        {  NONE,     NONE,    COPYREL,       PLT    },     // PDE
      };
      dispatch(ctx, table, i, rel, sym);
      break;
    }
    case R_RISCV_TLS_DTPMOD32:
    case R_RISCV_TLS_DTPMOD64:
    case R_RISCV_TLS_DTPREL32:
    case R_RISCV_TLS_DTPREL64:
    case R_RISCV_TLS_TPREL32:
    case R_RISCV_TLS_TPREL64:
      Error(ctx) << *this << ": unsupported relocation: " << rel;
      break;
    case R_RISCV_BRANCH:
    case R_RISCV_JAL:
      break;
    case R_RISCV_CALL:
    case R_RISCV_CALL_PLT:
      if (sym.is_imported)
        sym.flags |= NEEDS_PLT;
      break;
    case R_RISCV_GOT_HI20:
      sym.flags |= NEEDS_GOT;
      break;
    case R_RISCV_TLS_GOT_HI20:
    case R_RISCV_TLS_GD_HI20:
      Error(ctx) << *this << ": unsupported relocation: " << rel;
      break;
    case R_RISCV_PCREL_HI20:
    case R_RISCV_PCREL_LO12_I:
    case R_RISCV_PCREL_LO12_S:
    case R_RISCV_LO12_I:
    case R_RISCV_LO12_S:
    case R_RISCV_TPREL_HI20:
    case R_RISCV_TPREL_LO12_I:
    case R_RISCV_TPREL_LO12_S:
    case R_RISCV_TPREL_ADD:
    case R_RISCV_ADD8:
    case R_RISCV_ADD16:
    case R_RISCV_ADD32:
    case R_RISCV_ADD64:
    case R_RISCV_SUB8:
    case R_RISCV_SUB16:
    case R_RISCV_SUB32:
    case R_RISCV_SUB64:
    case R_RISCV_ALIGN:
      break;
    case R_RISCV_RVC_BRANCH:
    case R_RISCV_RVC_JUMP:
      break;
    case R_RISCV_RVC_LUI:
      Error(ctx) << *this << ": unsupported relocation: " << rel;
      break;
    case R_RISCV_RELAX:
      break;
    case R_RISCV_SUB6:
    case R_RISCV_SET6:
    case R_RISCV_SET8:
    case R_RISCV_SET16:
    case R_RISCV_SET32:
      Error(ctx) << *this << ": unsupported relocation: " << rel;
      break;
    case R_RISCV_32_PCREL: {
      Action table[][4] = {
        // Absolute  Local  Imported data  Imported code
        {  ERROR,    NONE,  ERROR,         ERROR },      // DSO
        {  ERROR,    NONE,  COPYREL,       PLT   },      // PIE
        {  NONE,     NONE,  COPYREL,       PLT   },      // PDE
      };
      dispatch(ctx, table, i, rel, sym);
      break;
    }
    default:
      Error(ctx) << *this << ": unknown relocation: " << rel;
    }
  }
}

} // namespace mold::elf