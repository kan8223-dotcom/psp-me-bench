# PSP ME Benchmark

世界初のPSP Media Engineデュアルベンチマーク。
シングルCPU vs デュアルCPU（ME並列）× クロック（222/333MHz）の4パターンで3種のベンチマークを実行し、MEによる実効性能向上を定量的に計測する。

## スクリーンショット

（結果画面のスクリーンショットをここに貼る）

## ベンチマーク結果

SC 222MHz基準の倍率（PSP-1000 / PSP-3000 共通）:

| ベンチ | SC 222 | DUAL 222 | SC 333 | DUAL 333 |
|--------|--------|----------|--------|----------|
| PI | x1.00 | x2.00 | x1.50 | **x3.01** |
| PRIME | x1.00 | x2.89 | x1.50 | **x4.33** |
| MANDEL | x1.00 | x2.07 | x1.50 | **x3.09** |
| **平均** | x1.00 | x2.32 | x1.50 | **x3.48** |

- PSP-1000とPSP-3000で性能差なし（同一Allegrexコア）
- チェックサムで全パターンの計算結果一致を検証済み

## ベンチマーク内容

| # | テスト | 内容 | パラメータ |
|---|--------|------|-----------|
| 1 | PI | ライプニッツ級数によるπ計算 | 100万項 |
| 2 | PRIME | 試行除算による素数カウント | 10万以下 |
| 3 | MANDEL | マンデルブロ集合計算 | 160×120, 最大64反復 |

DUAL時は計算範囲を半分に分割し、メインCPU（SC）とMEで並列実行。

## 操作方法

| ボタン | 機能 |
|--------|------|
| △ | 全自動実行（4パターン×3ベンチ） |
| ○ | 次の未完了ベンチを1個実行 |
| × | 中断 |
| START | CSV保存（`ms0:/me_bench.csv`） |
| SELECT | USBオン/オフ切替（XMBに戻らずPC接続） |
| R | スクリーンショット保存（`ms0:/mebench_N.bmp`） |

## セットアップ

### 必要なもの
- CFW導入済みPSP（PSP-1000 / 2000 / 3000）
- メモリースティック

### 手順

1. **kcall.prxの配置**

   `kcall.prx` をメモリースティックにコピー:
   ```
   ms0:/seplugins/kcall.prx
   ```

2. **sepluginの登録**

   `ms0:/seplugins/game.txt` を作成（または追記）:
   ```
   ms0:/seplugins/kcall.prx 1
   ```

3. **EBOOTの配置**

   `EBOOT.PBP` をメモリースティックにコピー:
   ```
   ms0:/PSP/GAME/MEBENCH/EBOOT.PBP
   ```

4. **PSPを再起動**（kcall.prxの読み込みに必要）

5. **CFWのクロック設定を「デフォルト」にする**（333MHz固定だとクロック変更APIが無視される）

6. **XMBからME Benchmarkを起動** → △で全自動実行

### kcall.prxについて
kcall.prxはTiny-MEライブラリがMedia Engineにアクセスするために必要なカーネルモードプラグインです。`tiny-me/build/kernel/kcall.prx` にビルド済みバイナリが含まれています。

## ビルド

### 必要なもの
- [PSPDEV toolchain](https://github.com/pspdev/pspdev)

### ビルド手順
```bash
make clean
make
```

`EBOOT.PBP` が生成されます。

## 技術詳細

- **ME通信**: uncached共有メモリでコマンド/ステータス/結果を受け渡し
- **タイマー**: `sceKernelGetSystemTimeLow()`（1MHz HWレジスタ、CPUクロック非依存）
- **ME側タイマー**: COP0 Count ($9)（CPU_CLOCK/2、カーネルモード）
- **クロック制御**: `scePowerSetClockFrequency()`（API上限333MHz）

## 動作確認済み
- PSP-1000 (FAT) — me_init_ret=3
- PSP-3000 (Slim+) — me_init_ret=2, T2テーブル

## ライセンス

本プロジェクトは [GPL-3.0](LICENSE) でライセンスされています。

Tiny-MEライブラリ (`tiny-me/`) は [MIT License](tiny-me/LICENSE.md) (c) 2025 m-c/d でライセンスされています。

## クレジット

- [Tiny-ME](https://github.com/pspdev/tiny-me) — PSP Media Engine Core Mapper Library by m-c/d
- PSPDev toolchain & community
