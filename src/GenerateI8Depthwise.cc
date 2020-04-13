/*
 * Copyright (c) Facebook, Inc. and its affiliates.
 * All rights reserved.
 * This source code is licensed under the BSD-style license found in the
 * LICENSE file in the root directory of this source tree.
 */
#include "./GenerateI8Depthwise.h"

#include <asmjit/asmjit.h>
#include <iostream>

#include "./CodeCache.h"
#include "./CodeGenHelpers.h"
#include "fbgemm/Utils.h"

namespace fbgemm {

namespace {
asmjit::JitRuntime& runtime() {
  static asmjit::JitRuntime rt; //< JIT Runtime for asmjit,
                                // depents on other static
                                // variables.  Required to prevent
                                // initialization order fiasco
  return rt;
}

// Controll access to runtime;
std::mutex rtMutex_;

// The hash depends on D, F, compute_a_sum, per_channel_quantization, remainder,
// prev_skip, next_skip, top_skip, bottom_skip, left_skip, and right_skip.
CodeCache<
    std::tuple<int, int, bool, bool, int, int, int, int, int, int, int>,
    GenI8Depthwise::jit_kernel_signature>
    codeCache_;
} // namespace

namespace x86 = asmjit::x86;

// c = a0 * b0 + a1 * b1 + a2 * b2 + a3 * b3
// A is in uint8_t
// B is in int8_t and pre-interleaved
// C is in int32_t and 4 registers have results in the following layout:
// c0_v:   c[0:4], c[16:20]
// c1_v:   c[4:8], c[20:24]
// c2_v:  c[8:12], c[24:28]
// c3_v: c[12:16], c[28:32]
static void genMaddEpi16xNPacked(
    x86::Emitter* e,
    x86::Ymm a[4],
    x86::Gp b,
    x86::Ymm c[4],
    x86::Ymm* a_sum,
    int n,
    int remainder,
    bool accumulation,
    x86::Ymm one_epi8,
    x86::Ymm one_epi16,
    x86::Ymm zero) {
  // Interleave inputs. Reuse a[1] and a[3] to save registers
  x86::Ymm a01_lo(0), a01_hi(1), a23_lo(a[1]), a23_hi(a[3]);
  e->vpunpcklbw(a01_lo, a[0], n == 1 ? zero : a[1]);
  if (remainder >= 8) {
    e->vpunpckhbw(a01_hi, a[0], n == 1 ? zero : a[1]);
  }
  if (n > 2) {
    e->vpunpcklbw(a23_lo, a[2], n == 3 ? zero : a[3]);
    if (remainder >= 8) {
      e->vpunpckhbw(a23_hi, a[2], n == 3 ? zero : a[3]);
    }
  }

  // Compute row_wise sum of A for row_offsets
  if (a_sum) {
    if (accumulation) {
      e->vpmaddubsw(a[0], a01_lo, one_epi8);
      e->vpaddsw(a_sum[0], a[0], a_sum[0]);

      if (remainder >= 8) {
        e->vpmaddubsw(a[2], a01_hi, one_epi8);
        e->vpaddsw(a_sum[1], a[2], a_sum[1]);
      }
    } else {
      e->vpmaddubsw(a_sum[0], a01_lo, one_epi8);
      if (remainder >= 8) {
        e->vpmaddubsw(a_sum[1], a01_hi, one_epi8);
      }
    }

    if (n > 2) {
      e->vpmaddubsw(a[0], a23_lo, one_epi8);
      e->vpaddsw(a_sum[0], a[0], a_sum[0]);

      if (remainder >= 8) {
        e->vpmaddubsw(a[2], a23_hi, one_epi8);
        e->vpaddsw(a_sum[1], a[2], a_sum[1]);
      }
    }
  }

  if (n > 2) {
    // Reusing a
    e->vpunpcklwd(a[0], a01_lo, a23_lo);
    e->vpunpckhwd(a[1], a01_lo, a23_lo);
    if (remainder >= 16) {
      e->vpunpcklwd(a[2], a01_hi, a23_hi);
      e->vpunpckhwd(a[3], a01_hi, a23_hi);
    }

    e->vpmaddubsw(a[0], a[0], x86::ymmword_ptr(b));
    e->vpmaddubsw(a[1], a[1], x86::ymmword_ptr(b, 32));
    if (remainder >= 16) {
      e->vpmaddubsw(a[2], a[2], x86::ymmword_ptr(b, 64));
      e->vpmaddubsw(a[3], a[3], x86::ymmword_ptr(b, 96));
    }

    if (accumulation) {
      e->vpmaddwd(a[0], a[0], one_epi16);
      e->vpaddd(c[0], c[0], a[0]);
      e->vpmaddwd(a[1], a[1], one_epi16);
      e->vpaddd(c[1], c[1], a[1]);

      if (remainder >= 16) {
        e->vpmaddwd(a[2], a[2], one_epi16);
        e->vpaddd(c[2], c[2], a[2]);
        e->vpmaddwd(a[3], a[3], one_epi16);
        e->vpaddd(c[3], c[3], a[3]);
      }
    } else {
      e->vpmaddwd(c[0], a[0], one_epi16);
      e->vpmaddwd(c[1], a[1], one_epi16);

      if (remainder >= 16) {
        e->vpmaddwd(c[2], a[2], one_epi16);
        e->vpmaddwd(c[3], a[3], one_epi16);
      }
    }
  } else {
    // Reusing a
    e->vpmaddubsw(a[0], a01_lo, x86::ymmword_ptr(b));
    e->vpmaddubsw(a[1], a01_hi, x86::ymmword_ptr(b, 32));

    if (accumulation) {
      e->vpmovsxwd(a[2], a[0].half());
      e->vpaddd(c[0], c[0], a[2]);
      e->vpmovsxwd(a[3], a[1].half());
      e->vpaddd(c[1], c[1], a[3]);

      if (remainder >= 16) {
        e->vextracti128(a[0].half(), a[0], asmjit::Imm(1));
        e->vpmovsxwd(a[0], a[0].half());
        e->vpaddd(c[2], c[2], a[0]);
        e->vextracti128(a[1].half(), a[1], asmjit::Imm(1));
        e->vpmovsxwd(a[1], a[1].half());
        e->vpaddd(c[3], c[3], a[1]);
      }
    } else {
      e->vpmovsxwd(c[0], a[0].half());
      e->vpmovsxwd(c[1], a[1].half());

      if (remainder >= 16) {
        e->vextracti128(a[0].half(), a[0], asmjit::Imm(1));
        e->vpmovsxwd(c[2], a[0].half());
        e->vextracti128(a[1].half(), a[1], asmjit::Imm(1));
        e->vpmovsxwd(c[3], a[1].half());
      }
    }
  }
}

GenI8Depthwise::jit_kernel_signature GenI8Depthwise::getOrCreate(
    int D,
    int S,
    bool compute_a_sum,
    bool per_channel_quantization,
    int remainder,
    int prev_skip,
    int next_skip,
    int top_skip,
    int bottom_skip,
    int left_skip,
    int right_skip) {
  std::tuple<int, int, bool, bool, int, int, int, int, int, int, int>
      kernelSig = std::make_tuple(
          D,
          S,
          compute_a_sum,
          per_channel_quantization,
          remainder,
          prev_skip,
          next_skip,
          top_skip,
          bottom_skip,
          left_skip,
          right_skip);

  return codeCache_.getOrCreate(kernelSig, [&]() -> jit_kernel_signature {
    asmjit::CodeHolder code;
    code.init(runtime().codeInfo());
    x86::Assembler assembler(&code);
    x86::Emitter* e = assembler.as<x86::Emitter>();

    x86::Gp a_addr = e->zdi();
    x86::Gp b_addr = e->zsi();
    x86::Gp c_addr = e->zdx();
    x86::Gp a_sum_addr = e->zcx();
    x86::Gp h = e->gpz(8);
    x86::Gp w = e->gpz(9);
    x86::Gp c_in = e->gpz(10);
    x86::Gp mask_addr = e->gpz(11);
    x86::Gp a_zero_point = e->gpz(12);
    x86::Gp b_zero_point_addr = e->gpz(13);
    x86::Gp ic_loop_count = e->gpz(14);
    x86::Gp a_addr_save = e->gpz(15);

    asmjit::FuncDetail func;
    func.init(asmjit::FuncSignatureT<
              void,
              const std::uint8_t*,
              const std::int8_t*,
              std::int32_t*,
              std::int32_t*,
              int,
              int,
              int,
              const int*,
              int,
              const std::int32_t*>(asmjit::CallConv::kIdHost));

    asmjit::FuncFrame frame;
    frame.init(func);

    frame.setDirtyRegs(
        x86::Reg::kGroupVec,
        asmjit::Support::bitMask(0, 1, 2, 3, 4, 5, 6, 7) |
            asmjit::Support::bitMask(8, 9, 10, 11, 12, 13, 14, 15));
    frame.setDirtyRegs(
        x86::Reg::kGroupGp,
        asmjit::Support::bitMask(8, 9, 10, 11, 12, 13, 14, 15));

    asmjit::FuncArgsAssignment args(&func);
    args.assignAll(
        a_addr,
        b_addr,
        c_addr,
        a_sum_addr,
        h,
        w,
        c_in,
        mask_addr,
        a_zero_point,
        b_zero_point_addr);

    args.updateFuncFrame(frame);
    frame.finalize();

    e->emitProlog(frame);
    e->emitArgsAssignment(frame, args);

    // Assign vector registers
    x86::Ymm a[4];
    x86::Ymm c[4];
    x86::Ymm a_sum[2];

    int vreg_id = 2; // reserve 2 for temp vreg
    for (int i = 0; i < 4; ++i, ++vreg_id) {
      a[i] = x86::Ymm(vreg_id);
    }
    for (int i = 0; i < 4; ++i, ++vreg_id) {
      c[i] = x86::Ymm(vreg_id);
    }
    if (compute_a_sum) {
      a_sum[0] = x86::Ymm(vreg_id);
      ++vreg_id;
      a_sum[1] = x86::Ymm(vreg_id);
      ++vreg_id;
    }
    x86::Ymm mask_vreg(vreg_id);
    constexpr int vlen = simd_info<inst_set_t::avx2>::WIDTH_32BIT_ELEMS;
    if (remainder != simd_info<inst_set_t::avx2>::WIDTH_BYTES) {
      ++vreg_id;
      e->vmovups(
          mask_vreg,
          x86::ymmword_ptr(
              mask_addr, (vlen - remainder / 4) % vlen * sizeof(int32_t)));
    }
    x86::Ymm one_epi8(vreg_id);
    if (compute_a_sum) {
      ++vreg_id;
      gen8BitVectorOne(e, one_epi8);
    }

    int K = 1;
    for (int i = 0; i < D; ++i) {
      K *= S;
    }
    x86::Ymm one_epi16(vreg_id);
    if (K > 2) {
      ++vreg_id;
      gen16BitVectorOne(e, one_epi16);
    }

    bool has_pad = prev_skip || next_skip || top_skip || bottom_skip ||
        left_skip || right_skip;
    bool need_zero = K % 4 == 3 || K % 4 == 1;
    // When out of registers, zero and A_zero_point_vreg need to share.
    bool recompute_zero = vreg_id == 15 && need_zero;

    x86::Ymm a_zero_point_vreg(vreg_id);
    if (!recompute_zero && has_pad) {
      e->movq(a_zero_point_vreg.half(), a_zero_point);
      e->vpbroadcastb(a_zero_point_vreg, a_zero_point_vreg.half());
    }
    if (vreg_id < 15) {
      ++vreg_id;
    }
    x86::Ymm zero(vreg_id);
    if (need_zero && (!recompute_zero || !has_pad)) {
      e->vxorps(zero, zero, zero);
    }

    // Assign scalar registers
    e->imul(w, c_in);
    e->imul(h, w);
    if (D >= 3) {
      e->mov(a_addr_save, w);
      e->imul(a_addr_save, S);
      e->sub(h, a_addr_save);
    }
    e->mov(a_addr_save, c_in);
    e->imul(a_addr_save, S);
    e->sub(w, a_addr_save);

    e->mov(ic_loop_count, c_in);
    e->add(ic_loop_count, asmjit::Imm(31));
    e->sar(ic_loop_count, asmjit::Imm(5));

    e->mov(a_addr_save, a_addr);
    asmjit::Label ic_loop_begin = e->newLabel(), ic_loop_end = e->newLabel();

    // main_loop == false: the last vector iteration across input channels
    for (bool main_loop : {true, false}) {
      if (main_loop) {
        e->bind(ic_loop_begin);
        e->dec(ic_loop_count);
        e->jle(ic_loop_end);
      }

      if (recompute_zero && has_pad) {
        e->movq(a_zero_point_vreg.half(), a_zero_point);
        e->vpbroadcastb(a_zero_point_vreg, a_zero_point_vreg.half());
      }

      int i = 0;
      // Iterate across the reduction (filter) dimension
      for (int f_t = 0; f_t < ((D == 2) ? 1 : S); ++f_t) {
        for (int f_h = 0; f_h < S; ++f_h) {
          for (int f_w = 0; f_w < S; ++f_w, ++i) {
            bool pad = false;
            if (D > 2) {
              if (f_t < prev_skip || f_t >= S - next_skip) {
                pad = true;
              }
            }
            if (f_h < top_skip || f_h >= S - bottom_skip || f_w < left_skip ||
                f_w >= S - right_skip) {
              pad = true;
            }

            // Load A
            if (pad) {
              e->vmovups(a[i % 4], a_zero_point_vreg);
            } else {
              if (!main_loop && remainder != 32) {
                e->vmaskmovps(a[i % 4], mask_vreg, x86::ymmword_ptr(a_addr));
              } else {
                e->vmovups(a[i % 4], x86::ymmword_ptr(a_addr));
              }
            }

            // Compute when we have 4 inputs or this is the last iteration
            if (i % 4 == 3 || i == K - 1) {
              if (i == K - 1 && (i / 4 * 4 == K - 3 || i / 4 * 4 == K - 1)) {
                if (recompute_zero && has_pad) {
                  e->vxorps(zero, zero, zero);
                }
              }

              genMaddEpi16xNPacked(
                  e,
                  a,
                  b_addr,
                  c,
                  compute_a_sum ? a_sum : nullptr,
                  /*n=*/std::min(K - i / 4 * 4, 4),
                  main_loop ? 32 : remainder,
                  /*accumulation=*/i / 4 > 0,
                  one_epi8,
                  one_epi16,
                  zero);

              if (i != K - 1) {
                e->add(b_addr, asmjit::Imm(32 * 4));
              } else if (main_loop) {
                e->add(b_addr, asmjit::Imm(32 * (K - i / 4 * 4 + 1) / 2 * 2));
              }

              if (K - i / 4 * 4 >= 3 && K - i / 4 * 4 <= 6) {
                for (int r = 0; r < (main_loop ? 4 : remainder / 8); ++r) {
                  e->vperm2f128(
                      a[r],
                      c[r % 2 * 2],
                      c[r % 2 * 2 + 1],
                      asmjit::Imm(r < 2 ? 0x20 : 0x31));
                }
                for (int r = 0; r < (main_loop ? 4 : remainder / 8); ++r) {
                  e->vmovaps(c[r], a[r]);
                }
              }
            }
            if (i != K - 1) {
              e->add(a_addr, c_in);
            }
          }
          if (i != K - 1) {
            e->add(a_addr, w);
          }
        }
        if (D >= 3 && i != K - 1) {
          e->add(a_addr, h);
        }
      }

      for (int r = 0; r < (main_loop ? 4 : remainder / 8); ++r) {
        e->vmovups(x86::ymmword_ptr(c_addr, r * 32), c[r]);
      }

      if (compute_a_sum) {
        if (per_channel_quantization) {
          e->vmovups(c[0], x86::ymmword_ptr(b_zero_point_addr));
        } else {
          e->vpbroadcastd(c[0], x86::dword_ptr(b_zero_point_addr));
        }
        e->vpmovsxwd(a[0], a_sum[0].half());
        e->vpmulld(a[0], a[0], c[0]);
        e->vmovups(x86::ymmword_ptr(a_sum_addr), a[0]);

        if (main_loop || remainder >= 8) {
          if (per_channel_quantization) {
            e->vmovups(c[0], x86::ymmword_ptr(b_zero_point_addr, 32));
          }
          e->vpmovsxwd(a[1], a_sum[1].half());
          e->vpmulld(a[1], a[1], c[0]);
          e->vmovups(x86::ymmword_ptr(a_sum_addr, 32), a[1]);
        }

        if (main_loop || remainder >= 16) {
          if (per_channel_quantization) {
            e->vmovups(c[0], x86::ymmword_ptr(b_zero_point_addr, 64));
          }
          e->vextracti128(a_sum[0].half(), a_sum[0], asmjit::Imm(1));
          e->vpmovsxwd(a_sum[0], a_sum[0].half());
          e->vpmulld(a_sum[0], a_sum[0], c[0]);
          e->vmovups(x86::ymmword_ptr(a_sum_addr, 64), a_sum[0]);
        }

        if (main_loop || remainder >= 24) {
          if (per_channel_quantization) {
            e->vmovups(c[0], x86::ymmword_ptr(b_zero_point_addr, 96));
          }
          e->vextracti128(a_sum[1].half(), a_sum[1], asmjit::Imm(1));
          e->vpmovsxwd(a_sum[1], a_sum[1].half());
          e->vpmulld(a_sum[1], a_sum[1], c[0]);
          e->vmovups(x86::ymmword_ptr(a_sum_addr, 96), a_sum[1]);
        }

        if (main_loop) {
          if (per_channel_quantization) {
            e->add(b_zero_point_addr, asmjit::Imm(128));
          }
          e->add(a_sum_addr, asmjit::Imm(128));
        }
      }

      if (main_loop) {
        e->add(c_addr, asmjit::Imm(128));
        e->add(a_addr_save, asmjit::Imm(32));
        e->mov(a_addr, a_addr_save);
        e->jmp(ic_loop_begin);

        e->bind(ic_loop_end);
      }
    }

    e->emitEpilog(frame);

    jit_kernel_signature fn;
    asmjit::Error err;
    {
      std::unique_lock<std::mutex> lock(rtMutex_);
      err = runtime().add(&fn, &code);
    }
    if (err) {
      std::cout << "Error: in fn add" << std::endl;
      return nullptr;
    }

    return fn;
  });
}

} // namespace fbgemm