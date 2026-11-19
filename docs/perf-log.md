# 性能记录（FLAC RVV 移植）

目标机：RISC-V RVV 板卡（8 核，VLEN=256），gcc 15.2.0，`-march=rv64gcv` 对 `-march=rv64gc`。
门禁：A 侧 = baseline 源码 + 本内核 + `-DRVV_KERNELS=1 -march=rv64gcv`；B 侧 = baseline 源码 +
`-march=rv64gc`。两侧除 `-march` 与内核开关外构建参数完全一致。

## 逐轮记录

| 轮 | 内核 | LMUL | 规模 | A(ms) | B(ms) | 收益 | 位精确 | 备注 |
|---|---|---|---|---|---|---|---|---|
| 1 | `FLAC__lpc_restore_signal`（解码侧 LPC 重建） | e32m1 | 15 s 44.1k 立体声解码 | 63 | 59–61 | 0.94x（回退） | 通过（300 组） | 样本间强依赖，每样本只能做 order 次点积，向量建立开销 > 收益；已撤除 |
| 2 | `FLAC__lpc_compute_residual_from_qlp_coefficients`（编码侧残差） | e32m4 | order 8 / 12，len 65536 | 2.14→0.11 / 5.19→0.15 | — | 内核级 20.2x / 34.1x | 通过（11 长度 × order 1–32 × shift 4–15，`total=df8d9c26580eec53`） | 样本间无依赖，按样本向量化 |
| 2 | 同上，端到端 `flac -8` | — | 15 s 44.1k 立体声编码 | 219–232（best 219） | 498–516（best 498） | 2.27x | 通过（`.flac` 逐字节相同 `745c885e…`） | PCM 解码 md5 与原始 wav 相同 |

## 内核说明

`src/src/libFLAC/rvv/lpc_res_rvv.h`：`flac_lpc_residual_rvv()`

- 对每个输出样本 i，一次连续装载 `data[i-order .. i-1]`（`vle32.v`，`vl = order`），
  与 `qlp_coeff` 广播向量 `vmul.vx` 乘后累加到 `acc`，截断到标量宽度，
  再 `vsra.vx` 算术右移 `lp_quantization` 位，`vse32.v` 写回 `residual[i]`。
- LMUL=4（`vint32m4_t`，VLEN=256 时一次 32 车道）；`order > vl` 时用 `vsetvl` 分块累加，
  因此 order 1–32 全覆盖，无需尾部特判。
- 截断必须在移位前做：标量路径的 `sum` 是 `FLAC__int32`，累加过程就已截断；
  `vmv.v.x` 到 `vint32m4_t` 的隐式转换等价于同一截断，二者逐位一致。

## 已知口径问题（已修正）

`projects/flac-rvv/src` 曾是从**另一版本**上游树复制而来（VORBIS_COMMENT 里的 vendor 串为
`reference libFLAC 1.5.0 20250211`，而 `baseline/` 是 `git-59fa340 20261028`），导致整文件
字节不可比。已改为「把 `baseline/` 整树复制为 A 侧工作树、只打本内核补丁」再构建，
两侧 `.flac` 现已逐字节相同。

## 复现

```bash
# B 侧（标量）
cmake -S baseline -B build/cpu -DCMAKE_BUILD_TYPE=Release \
      -DCMAKE_C_FLAGS="-march=rv64gc -O2" -DBUILD_PROGRAMS=ON -DBUILD_TESTING=OFF \
      -DWITH_OGG=OFF -DINSTALL_MANPAGES=OFF -DBUILD_DOCS=OFF
make -C build/cpu -j8
# A 侧（baseline 副本 + 内核）
cp -a baseline srcA && <把 rvv/lpc_res_rvv.h 与 lpc.c 的两处改动放进 srcA>
cmake -S srcA -B build/A -DCMAKE_BUILD_TYPE=Release \
      -DCMAKE_C_FLAGS="-march=rv64gcv -O2 -DRVV_KERNELS=1" -DBUILD_PROGRAMS=ON \
      -DBUILD_TESTING=OFF -DWITH_OGG=OFF -DINSTALL_MANPAGES=OFF -DBUILD_DOCS=OFF
make -C build/A -j8
# A/B
for bin in build/cpu/src/flac/flac build/A/src/flac/flac; do
  $bin --totally-silent -f -8 -o /tmp/out-$(basename $(dirname $(dirname $bin))).flac test.wav
done
cmp /tmp/out-cpu.flac /tmp/out-A.flac && echo "bit-identical"
```
