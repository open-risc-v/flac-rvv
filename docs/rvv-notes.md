# RVV 编码笔记 —— `media-libs/flac`

> 每次踩坑写一条：**现象 / 原因 / 正确写法 / 怎么发现的**。这份文件是交接文档。

## 通用纪律（沿用 libspeex-rvv 的结论，先照做再质疑）

1. **`vsetvl` 放循环内**，用 `__riscv_vsetvl_e<sew>m<lmul>(avail)`；不要把 `vlmax` 当 `vl` 用。
   尾部靠它收敛，循环外设一次是典型错误。
2. **`vmerge` 操作数序**是 `(mask, false_val, true_val, vl)`——顺序写反不会报错，只会安静地错。
3. **`vredsum` 参数序**是 `(dst_carrier, vector, scalar_init, vl)`，结果在 `vs1[0]`。
4. **尾部策略**：`_tu`（tail-agnostic）与 `ta` 混用会在"整寄存器写回内存"时写入脏 lane；
   用 `vle/vse` 显式带 `vl` 最安全。
5. **`vbool*` 类型宽度随 LMUL 变**（m1→`vbool8_t`，m8→`vbool1_t`），跨 LMUL 传 mask 必须显式转换。
6. **数组不能存向量类型**：`vint16m1_t a[4]` 会强制溢出到内存，用多个局部变量展开。
7. **`vlse` 的 stride 是字节**，不是元素数；且实测比 `vle` 慢 3–5 倍，能重排内存就先重排。

## 本包特有

（示例格式，实际填写时删掉本行）

```
现象  : 某内核在 VLEN=256 下正确，在 VLEN=128 下尾块错
原因  : 误用 vsetvlmax_e8m8 固定 vl，尾块多算
写法  : while (i<n) { vl = vsetvl_e8m1(n-i); ... }
发现  : 毒化矩阵 vlen=128 跑 diff_kernels
```

## 待查清单

- [ ] 无 RVV 目标上的 dispatch 回退（`qemu-riscv64 -cpu rv64` 无 v 扩展）
- [ ] VLEN 128 / 256 / 512 三档毒化
- [ ] `-march` 不含 `zvl*` 时行为一致

## 统一门禁入口（2026-11-25）

`test/cases/01_flac_roundtrip.sh` 把 flac 的编解码接进了与其余包相同的 `STAGE/SIDE/BIN` 口径：`$BIN/flac_harness` 编码 + 解码，产物是 `flac_stream.bin` 与 `hash.txt`。板子上用 `RVV_A=<ab-a> RVV_B=<ab-b> bash test/run_ab.sh` 直接跑（`sh` 会因 `shopt` 报错，必须 bash）。

两侧构建上游一致、只差内核宏：

```
cmake -S src      -B build/ab-a -DCMAKE_BUILD_TYPE=Release -DINSTALL_MANPAGES=OFF -DHAVE_GIT= \
  -DBUILD_PROGRAMS=OFF -DBUILD_EXAMPLES=OFF -DBUILD_TESTING=OFF -DBUILD_DOCS=OFF -DWITH_OGG=OFF \
  -DCMAKE_C_FLAGS="-march=rv64gcv -O2 -DRVV_KERNELS=1" -DCMAKE_INSTALL_PREFIX=$PWD/build/ab-a/prefix
cmake -S baseline -B build/ab-b ... 同上，但 C_FLAGS 去掉 -DRVV_KERNELS=1
# 再做探针：-DPKG_PREFIX=<该侧 prefix> -D_USE_RVV=ON -DRVV_MARCH=rv64gcv
```

### 坑：`HAVE_GIT` 让 A/B 码流天然不同

第一次跑门禁 A/B 不一致：两侧 PCM 哈希相同，但码流 5561 B vs 5567 B。`cmp` 定位到第 73 字节的 Vorbis comment vendor 串：

- A（`src/`，目录里没有 `.git`）→ `reference libFLAC 1.5.0 20250211`
- B（`baseline/`，带 `.git`）→ `reference libFLAC git-59fa340 20261028`

`src/CMakeLists.txt` 里 `find_program(HAVE_GIT git)` 后会用 `git describe` 拼版本号；有没有 `.git` 决定走哪条分支。两侧加 `-DHAVE_GIT=`（空值让 `if(HAVE_GIT)` 为假）后码流逐字节一致。

顺带用独立探针验证了内核本身确实位精确：直接调用 `FLAC__lpc_compute_residual_from_qlp_coefficients`，order 1..32 全跑，两侧残差 FNV 完全相同（`order=1 69b04eaff0f50d1e`、`order=32 efe89455fc27326d`）。
