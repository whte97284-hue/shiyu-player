# Hi-Res 库第二轮实测回传数据

## 0. 环境与代码版本

- 代码: `59f2dd4 增量扫描:按「路径 + 大小 + 修改时间」缓存解析结果,未变化文件零磁盘读取`
- 源文件 md5(前8): test_hires_runner.cpp=`faa97e26` scan_cache.cpp=`76bd0843`
  audio_ingestion_pipeline.cpp=`d2d3f9b8` library_audit.cpp=`8d93d563`
- 工作目录: `F:\软件\酷狗最新版转换器\KugouConverterWeb`(相对路径以此为准)
- 已 `rm -f player_core/test/hires_scan.cache`(运行前确认不存在)

> ⚠️ **未执行重启电脑**。因此**这不是冷启动**——两次扫描都是页缓存全热状态。
> 下面的耗时数字**不能代表机械盘冷启动性能**,请勿据此下结论。

---

## 1. 梯度 2(首次全量扫描)完整输出

```
▶ 【梯度 2】240GB 全母带库多线程异步并发扫描与吞吐量压测 (AudioIngestionPipeline):
  - 扫描源路径: F:\ZiYuan\Hires
  - 并发线程数: 8 (自适应 CPU 物理/逻辑核心)
  - 惰性策略: 已启用 (封面按需提取，仅解析音频流物理帧头与标签)
  [PASS] 全库 StartAsync 立即非阻塞触发
    [异步压测进度] 500 / 3708 (13.5%) - 正在解析: 01_19_自転車の二人.flac
    [异步压测进度] 1000 / 3708 (27.0%) - 正在解析: 杨幂-明月.flac
    [异步压测进度] 1500 / 3708 (40.5%) - 正在解析: 神原駿河(沢城みゆき) - 歌物語-〈物語〉シリーズ主題歌集-.flac
    [异步压测进度] 2000 / 3708 (53.9%) - 正在解析: 华语群星-载歌载舞.flac
    [异步压测进度] 2500 / 3708 (67.4%) - 正在解析: (14) [陳奕迅] 垃圾.flac
    [异步压测进度] 3000 / 3708 (80.9%) - 正在解析: (10) [陳奕迅] 乜嘢啫.flac
    [异步压测进度] 3500 / 3708 (94.4%) - 正在解析: 2.Lonely.flac
    [异步压测进度] 3708 / 3708 (100.0%) - 正在解析: 11.应该不应该.flac

  [压测结果指标审计]:
  - 全局扫描物理文件总数: 3708
  - 成功解析入库音频总数: 3679
  - 解析失败数: 1 (逐文件原因见 library_audit 输出 CSV 的 error 列)
  - 跳过的非音乐文件 (视频等): 28
  - 其中 DSD (DSF/DFF) 成功入库: 51 首
  - CUE 拆分曲目: 267, 入库曲目总数: 3934
  - 解析阶段真实读盘: 363 MB / 库体积 185 GB
  - 存储类型: 机械盘 (注意:若刚运行过 library_audit,文件头已在系统缓存中,本次耗时不代表冷启动)
  - 总耗时 (Wall Time): 0.215437 秒
  - 平均解析吞吐量: 17211.544174 首/秒
  - 增量缓存: 命中 0 / 实际解析 3708 (首次全量扫描)
  [PASS] 扫描到有效音频数量 >= 3000 首 (3708 首)
  [PASS] 入库成功数 >= 3000 首 (3679 首)
  [PASS] 单机并发吞吐量 >= 50 首/秒 (17211.544174 首/秒)
```

**关键数字**

| 指标 | 数值 |
|---|---|
| 总耗时(梯度2) | **0.215437 s** |
| 缓存命中 / 实际解析 | **0 / 3708** |
| 解析阶段读盘 | **363 MB**(库体积 185 GB,读放大 0.19%) |
| 吞吐 | 17211.5 首/秒 |

---

## 2. 梯度 2b(增量扫描)完整输出

```
▶ 【梯度 2b】增量扫描 (库未变化,复用上一轮写入的缓存):
  - 目录枚举: 0.032964 秒, 总耗时: 0.104417 秒
  - 缓存命中: 3708, 实际解析: 0, 解析阶段读盘: 0 KB
  [PASS] 增量扫描曲目数与全量扫描一致 (3934 首)
  [PASS] 库未变化时全部命中缓存 (0 个未命中)
```

**关键数字**

| 指标 | 数值 |
|---|---|
| 目录枚举 | 0.032964 s |
| 总耗时(梯度2b) | **0.104417 s** |
| 缓存命中 / 实际解析 | **3708 / 0** |
| 解析阶段读盘 | **0 KB** |

缓存文件: `player_core/test/hires_scan.cache`,1,318,147 字节。

---

## 3. 梯度 3 与总结

```
  - 格式分布: FLAC = 3383 首, WAV = 343 首, MP3 = 57 首
  - 检索 '陈奕迅' 命中: 1240 首 (耗时 0.452500 ms)
  - 拼音首字母 'CYX' 命中: 1240 首 (耗时 0.493900 ms)
  - 检索 'Yiruma' 命中: 17 首
  - 原始总音轨数: 3934 首 -> 智能去重聚合后独立作品数: 2916 首
  最终发烧压测物理自证统计: 通过 29 项 / 失败 0 项
```

---

## 4. library_audit 最后一行汇总

```
files=3708 ok=3679 fail=29 enumerate=0.02s parse=0.27s (13962 files/s, single thread) library=185.5GB read=363.2MB (0.1912%)
```

---

## 5. CSV 中 error 列非空的所有行(共 30 行)

分类统计:**28 个 `video_file` + 1 个 `encrypted_kgm`(ok=1) + 1 个 `encrypted_unknown`(卫兰)**

### 5.1 `encrypted_unknown` — 唯一真正的解析失败

```
ok=0  fmt=  err=encrypted_unknown  size=24.96MB
F:\ZiYuan\Hires\粤语\卫兰-我怀念的你.flac
```

### 5.2 `encrypted_kgm`(ok=1,已正常入库,仅标记为密文容器)

```
ok=1  fmt=KGMA  err=encrypted_kgm  size=7.71MB
F:\ZiYuan\Hires\孤独摇滚\KugouMusic\Vaundy - 裸の勇者 (赤身勇者).kgm
```

### 5.3 `video_file`(28 个 MP4 视频)

| size | path |
|---|---|
| 99.35MB | `压缩包\[合集]八爷\其他\＜NHK＞2020応援ソング「パプリカ」ダンス ミュージックビデオ.mp4` |
| 45.80MB | `压缩包\[合集]八爷\其他\＜NHK＞2020応援ソング「パプリカ」世界観ミュージックビデオ.mp4` |
| 368.00MB | `孤独摇滚\[ANi] 孤獨搖滾！ - 10 [1080P][Baha][WEB-DL][AAC AVC][CHT].mp4` |
| 372.47MB | `孤独摇滚\[ANi] 孤獨搖滾！ - 11 [1080P][Baha][WEB-DL][AAC AVC][CHT].mp4` |
| 378.09MB | `孤独摇滚\[ANi] 孤獨搖滾！ - 12 [1080P][Baha][WEB-DL][AAC AVC][CHT].mp4` |
| 412.39MB | `孤独摇滚\[Lilith-Raws] Bocchi the Rock! - 09 [Baha][WEB-DL][1080p][AVC AAC][CHT][MP4].mp4` |
| 377.07MB | `孤独摇滚\[Lilith-Raws] Bocchi the Rock! - 12 [Baha][WEB-DL][1080p][AVC AAC][CHT][MP4].mp4` |
| 371.61MB | `孤独摇滚\[NC-Raws] 孤獨搖滾！ - 11 (Baha 1920x1080 AVC AAC MP4) [79A23030].mp4` |
| 377.23MB | `孤独摇滚\[NC-Raws] 孤獨搖滾！ - 12 (Baha 1920x1080 AVC AAC MP4) [3B266FF3].mp4` |
| 8.53MB | `孤独摇滚\「有什么不好」#结束乐队.mp4` |
| 1110.27MB | `孤独摇滚\KugouMusic\[2022.12.28] …\孤独摇滚\4K60帧op重置\856286816-1-208_chf3_prob3.mp4` |
| 409.44MB | `…\动漫资源\[Sakurato] Bocchi the Rock! [01][AVC-8bit 1080p AAC][CHS].mp4` |
| 2738.04MB | `…\动漫资源\[Sakurato] Bocchi the Rock! [01][AVC-8bit 1080p AAC][CHS]_prob3.mp4` |
| 414.93MB | `…\动漫资源\[Sakurato] Bocchi the Rock! [02][AVC-8bit 1080p AAC][CHS].mp4` |
| 409.50MB | `…\动漫资源\[Sakurato] Bocchi the Rock! [02][AVC-8bit 1080p AAC][CHS]_197444083.mp4` |
| 2846.64MB | `…\动漫资源\[Sakurato] Bocchi the Rock! [02][AVC-8bit 1080p AAC][CHS]_prob3.mp4` |
| 412.99MB | `…\动漫资源\[Sakurato] Bocchi the Rock! [03][AVC-8bit 1080p AAC][CHS].mp4` |
| 2762.62MB | `…\动漫资源\[Sakurato] Bocchi the Rock! [03][AVC-8bit 1080p AAC][CHS]_906350691.mp4` |
| 2763.01MB | `…\动漫资源\[Sakurato] Bocchi the Rock! [03][AVC-8bit 1080p AAC][CHS]_prob3.mp4` |
| 375.51MB | `…\动漫资源\[Sakurato] Bocchi the Rock! [04][AVC-8bit 1080p AAC][CHT].mp4` |
| 366.90MB | `…\动漫资源\[Sakurato] Bocchi the Rock! [05][AVC-8bit 1080p AAC][CHS].mp4` |
| 355.80MB | `…\动漫资源\[Sakurato] Bocchi the Rock! [06][AVC-8bit 1080p AAC][CHS].mp4` |
| 378.61MB | `…\动漫资源\[Sakurato] Bocchi the Rock! [07][AVC-8bit 1080p AAC][CHS].mp4` |
| 380.44MB | `…\动漫资源\[Sakurato] Bocchi the Rock! [08][AVC-8bit 1080p AAC][CHS].mp4` |
| 398.16MB | `…\动漫资源\[Sakurato] Bocchi the Rock! [09][AVC-8bit 1080p AAC][CHT].mp4` |
| 355.27MB | `…\动漫资源\[Sakurato] Bocchi the Rock! [10][AVC-8bit 1080p AAC][CHS].mp4` |
| 365.18MB | `…\动漫资源\[Sakurato] Bocchi the Rock! [11][AVC-8bit 1080p AAC][CHS].mp4` |
| 302.54MB | `…\动漫资源\[Sakurato] Bocchi the Rock! [12][AVC-8bit 1080p AAC][CHS].mp4` |

### 5.4 上一轮失败的 5 个文件已全部修复

| 文件 | 上轮 | 本轮 |
|---|---|---|
| `86\…\FLAC\02.Hands Up to the Sky.flac` | ok=0 (打开失败) | **ok=1 FLAC** |
| `Game of Thrones…\01. Ramin Djawadi - Main Title….flac` | ok=0 (打开失败) | **ok=1 FLAC** |
| `__MACOSX\…\._01/_22/_44-*.flac` ×3 | ok=0 | **已从扫描中过滤**(命中 0 行) |

---

## 6. 卫兰文件最后 1KB 十六进制

文件: `F:\ZiYuan\Hires\粤语\卫兰-我怀念的你.flac`
大小: **26,170,393** 字节 | mtime: 2022-12-03

**文件头前 64 字节**(注意:不是 `fLaC`)

```
00000000: 4a f8 46 a5 5c ab a2 32 ec dc 73 32 8e 85 a0 39
00000010: 05 37 21 1c fa a9 e2 27 ba 29 11 ce de 20 c7 15
00000020: ab 8d d9 dd 30 36 9d ca 12 f0 96 d5 3e cd 21 d4
00000030: a6 9a 30 10 d3 6b 80 8c a7 f9 9b 23 8c 5a 55 56
```

**最后 1024 字节**(起始偏移 26169369 = 0x18F5019)

```
018f5019  76 1d 0a 61 24 99 24 89 43 09 98 61 87 90 a1 81 24 cc 28 67 32 4a 65 27 42 4d 29 d9 d0 ca 64 90
018f5039  a6 4e 4d a6 91 29 21 24 84 90 a6 4e 18 64 d5 6f 39 81 21 99 39 4b 04 32 49 94 a0 44 30 d2 20 44
018f5059  0a 19 0c a1 81 43 f9 3f b7 cc 99 84 4a 42 d0 cc 92 f2 4b 93 0c 99 81 f2 85 c9 b3 30 c9 0f 0c 24
018f5079  b4 92 e7 a5 2c 92 25 03 3d 38 94 92 49 4a 18 4b 42 91 c8 65 0a 14 33 e6 11 33 f1 0e c9 92 7e 49
018f5099  94 ca 12 13 24 cc 94 c2 65 0b 3a 11 0a 59 97 a4 cc a5 33 24 99 94 92 14 a1 9f a6 11 33 39 86 16
018f50b9  61 68 5e 12 06 05 29 09 28 14 28 19 50 c2 20 60 7b 98 50 e7 c9 0c d3 02 99 24 9c a1 43 09 25 08
018f50d9  81 4b 92 e1 42 94 0c d9 33 88 61 99 cb 42 4c c2 71 0c 32 4f ce f4 c0 a5 27 09 26 74 92 09 85 a6
018f50f9  67 c2 81 43 98 61 04 cb c9 c2 94 a1 42 e4 e8 50 28 60 52 19 86 18 64 ca 44 89 6c b4 89 42 99 4d
018f5119  c3 c2 85 33 24 90 e9 21 26 61 24 cc 89 43 e4 e6 50 29 d9 93 cc cc 94 a1 21 42 4e 50 c3 30 cc 99
018f5139  84 42 94 92 65 22 12 10 ca 52 99 4c 99 42 81 73 e6 53 cc cf f9 e7 d2 86 12 4c a1 43 0c b9 29 42
018f5159  97 21 24 93 08 66 12 19 26 4a f4 28 61 4b 92 96 91 28 50 a7 28 19 10 c2 42 18 50 88 5d e4 c2 94
018f5179  93 90 9d 32 18 12 59 94 2f 26 12 52 48 60 61 b2 61 84 94 e6 50 c3 21 25 28 61 24 cc 88 66 98 50
018f5199  a1 93 30 88 61 49 25 22 11 0c a6 9f 4a 04 4c 92 67 99 74 28 53 94 22 11 09 21 42 20 50 b2 21 04
018f51b9  22 16 50 a1 43 21 94 a4 c3 02 e1 de 65 02 81 43 d3 0c a1 40 ca 4c c0 a1 42 96 bd 22 06 04 9a 53
018f51d9  02 92 10 c2 25 0a 59 26 fc c3 09 24 99 86 52 99 34 93 c2 71 29 f3 08 49 93 de 61 49 26 44 a1 4f
018f51f9  43 0c 30 93 32 9d 27 4a 50 cc 92 49 94 a1 66 61 86 65 09 27 b9 42 49 09 0c 24 30 f1 28 50 2f 29
018f5219  42 99 33 0a 05 3e 16 81 9c cc 24 91 2d f3 c9 4f 43 24 c9 ff ff ff ff ff ff ff ff ff ff ff ff ff
018f5239  ff ff ff ff ff ff ff ff ff ff ff ff ff ff ff ff ff ff ff ff ff ff ff ff ff ff ff ff ff ff ff ff
018f5259  ff ff ff ff ff ff ff ff ff ff ff ff ff ff ff ff ff ff ff ff ff ff ff ff ff ff ff ff ff ff ff ff
018f5279  ff ff ff ff ff ff ff ff ff ff ff ff ff ff ff ff ff ff ff ff ff ff ff ff ff ff ff ff ff ff ff ff
018f5299  ff ff ff ff ff ff ff ff ff ff ff ff ff ff ff ff ff ff ff ff ff ff ff ff ff ff ff ff ff ff e0 5f 58 ff f8
018f52b9  c9 18 e0 a5 90 a9 00 00 00 00 00 00 f8 d5 ff f8 c9 18 e0 a5 91 ae 00 00 00 00 00 00 6f a0 ff f8
018f52d9  c9 18 e0 a5 92 a7 00 00 00 00 00 00 56 3a ff f8 c9 18 e0 a5 93 a0 00 00 00 00 00 00 c1 4f ff f8
018f52f9  c9 18 e0 a5 94 b5 00 00 00 00 00 00 25 0e ff f8 c9 18 e0 a5 95 b2 00 00 00 00 00 00 b2 7b ff f8
018f5319  c9 18 e0 a5 96 bb 00 00 00 00 00 00 8b e1 ff f8 c9 18 e0 a5 97 bc 00 00 00 00 00 00 1c 94 ff f8
018f5339  c9 18 e0 a5 98 91 00 00 00 00 00 00 c3 66 ff f8 c9 18 e0 a5 99 96 00 00 00 00 00 00 54 13 ff f8
018f5359  c9 18 e0 a5 9a 9f 00 00 00 00 00 00 6d 89 ff f8 c9 18 e0 a5 9b 98 00 00 00 00 00 00 fa fc ff f8
018f5379  c9 18 e0 a5 9c 8d 00 00 00 00 00 00 1e bd ff f8 c9 18 e0 a5 9d 8a 00 00 00 00 00 00 89 c8 ff f8
018f5399  c9 18 e0 a5 9e 83 00 00 00 00 00 00 b0 52 ff f8 c9 18 e0 a5 9f 84 00 00 00 00 00 00 27 27 ff f8
018f53b9  c9 18 e0 a5 a0 39 00 00 00 00 00 00 61 7f ff f8 c9 18 e0 a5 a1 3e 00 00 00 00 00 00 f6 0a ff f8
018f53d9  c9 18 e0 a5 a2 37 00 00 00 00 00 00 cf 90 ff f8 c9 18 e0 a5 a3 30 00 00 00 00 00 00 58 e5 ff f8
018f53f9  c9 18 e0 a5 a4 25 00 00 00 00 00 00 bc a4 ff f8 79 18 e0 a5 a5 01 d7 42 00 00 00 00 00 00 3a 09
```

**尾部结构观察(供参考,未做结论)**

1. 文件头不是 `fLaC`,是 `4a f8 46 a5 5c ab a2 32 …` → 不是裸 FLAC。
2. 偏移 `0x18F5234` 起有约 0x60 字节的 `FF` 填充。
3. `0x18F52AB` 起出现**严格 16 字节周期**的块结构:

   ```
   FF F8 | C9 18 E0 A5 | <2字节计数器> | 00 00 00 00 00 00 | <2字节>
   ```
   计数器高字节 0x90 → 0xA5 逐个递增(共 22 个块,最后一拍为 `79 18 E0 A5 A5 01`)。
4. `FF F8` 恰好是 FLAC「固定块大小帧同步」的位模式(14 bit sync + blocking strategy)。

> 以上仅为字节结构描述,具体是什么容器/加密请由你判断。
