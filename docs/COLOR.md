# 真实照片上的色相误差（饱和度算子对照）

由 `scripts/report_color.py` 生成。输入：`/usr/share/backgrounds` 里三张 480×270 缩图；测得的是 `hdr_convert --sat` 输出的 16 位线性图与输入的 **CIELAB 色相角**差（`atan2(b*, a*)`）。

| 照片 | sat | 算子 | 中位 | p99 | 最大 |
|---|---|---|---|---|---|
| Clouds_by_Tibor_Mokanszki | 1.6 | 线性锚定（旧） | 0.88° | 2.45° | 11.14° |
| Clouds_by_Tibor_Mokanszki | 1.6 | Lab 缩放（默认） | 0.01° | 0.06° | 0.88° |
| Clouds_by_Tibor_Mokanszki | 2.0 | 线性锚定（旧） | 1.21° | 3.38° | 13.22° |
| Clouds_by_Tibor_Mokanszki | 2.0 | Lab 缩放（默认） | 0.00° | 0.06° | 0.68° |
| Fuji_san_by_amaral | 1.6 | 线性锚定（旧） | 1.40° | 7.84° | 10.52° |
| Fuji_san_by_amaral | 1.6 | Lab 缩放（默认） | 0.01° | 0.43° | 1.31° |
| Fuji_san_by_amaral | 2.0 | 线性锚定（旧） | 2.37° | 9.22° | 13.25° |
| Fuji_san_by_amaral | 2.0 | Lab 缩放（默认） | 0.01° | 0.41° | 1.02° |
| Monument_valley_by_orbitelambda | 1.6 | 线性锚定（旧） | 1.08° | 10.67° | 11.45° |
| Monument_valley_by_orbitelambda | 1.6 | Lab 缩放（默认） | 0.00° | 0.02° | 0.24° |
| Monument_valley_by_orbitelambda | 2.0 | 线性锚定（旧） | 1.56° | 12.34° | 13.70° |
| Monument_valley_by_orbitelambda | 2.0 | Lab 缩放（默认） | 0.00° | 0.01° | 0.24° |

结论：Lab 缩放的中位误差 **0.01°**（按构造应为 0，残差全是 float 往返），线性锚定的 p99 最差 **12.34°**。

为什么线性锚定会偏：`y + (c-y)*s` 只保证「三通道同比偏离 Rec.709 亮度」，而 Lab 的色相是 `(a*, b*)` 的夹角；亮度锚定在非线性变换下会把色相推走，越接近色域边、`s` 越大越明显（Monument 的岩壁/天空渐变最吃亏）。

上屏（Metal）：`src_msl_hdr` 的 `s2a_sat()` 已移植同一套 Lab 算式（同一组常数、同一个「中性色短路」、同样 14 次二分的色域守卫），`t_hdr_gpu_mirror` 用 30 组样本逐通道对照两侧，最大差 0.000000。

仍未验证的两条：MSL 从未被 `xcrun metal` 真编过（GitHub CI 随账号停用而不可触发，只剩 `scripts/check_msl.py` 的静态守卫），`s2a_sat` 的 GPU 端耗时也没有实测（它只在 `S2A_HDR_SAT != 1` 时进入，且仅当缩放把某通道推成负值才走二分）。
