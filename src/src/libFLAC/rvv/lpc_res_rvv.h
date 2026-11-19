/* FLAC LPC 残差计算的 RVV 内核（编码侧，样本间无依赖）。
 *
 * 语义与 FLAC__lpc_compute_residual_from_qlp_coefficients 的 C 版逐位等价：
 *   residual[i] = data[i] - (Σ_j qlp_coeff[j] * data[i-1-j]) >> lp_quantization
 * 全部 int32 模 2^32 回绕；加法在模 2^32 下可结合，与标量累加顺序无关。
 * 按样本向量化：每个 j 一次连续装载 + 广播乘 + 累加，流水线里没有依赖链。
 */
#ifndef FLAC_LPC_RES_RVV_H
#define FLAC_LPC_RES_RVV_H

#include <stdint.h>
#include <riscv_vector.h>

static void
flac_lpc_residual_rvv(const int32_t *data, uint32_t data_len,
                      const int32_t *qlp_coeff, uint32_t order,
                      int lp_quantization, int32_t *residual)
{
  uint32_t i = 0;

  while (i < data_len)
  {
    size_t vl = __riscv_vsetvl_e32m4(data_len - i);
    vint32m4_t acc = __riscv_vmv_v_x_i32m4(0, vl);
    uint32_t j;

    for (j = 0; j < order; j++)
    {
      vint32m4_t vd = __riscv_vle32_v_i32m4(data + i - 1 - j, vl);

      acc = __riscv_vadd_vv_i32m4(acc,
              __riscv_vmul_vx_i32m4(vd, qlp_coeff[j], vl), vl);
    }

    {
      vint32m4_t vres = __riscv_vle32_v_i32m4(data + i, vl);

      vres = __riscv_vsub_vv_i32m4(vres, __riscv_vsra_vx_i32m4(acc, lp_quantization, vl), vl);
      __riscv_vse32_v_i32m4(residual + i, vres, vl);
    }

    i += vl;
  }
}

#endif
