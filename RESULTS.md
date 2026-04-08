# PSP ME Benchmark Results — 2026-04-05 (Final)

## テスト環境
- PSP-1000 / PSP-3000 (CFW, kcall.prx seplugin)
- MECC (meLibDefaultInit)
- ベンチマーク: PI (ライプニッツ級数 100万項), PRIME (試行除算 10万), MANDEL (160x120 iter64)
- タイマー: sceKernelGetSystemTimeLow() (1MHz HWレジスタ, クロック非依存)

## PSP-1000 最終結果 (me_init_ret=3, FATテーブル)

| ベンチ | SC 222MHz | DUAL 222MHz | SC 333MHz | DUAL 333MHz |
|--------|-----------|-------------|-----------|-------------|
| PI | 245,550 us | 122,790 us | 163,452 us | 81,720 us |
| PRIME | 164,686 us | 57,057 us | 109,652 us | 38,010 us |
| MANDEL | 31,273 us | 15,116 us | 20,864 us | 10,108 us |

## PSP-3000 最終結果 (me_init_ret=2, T2 Slim+テーブル)

| ベンチ | SC 222MHz | DUAL 222MHz | SC 333MHz | DUAL 333MHz |
|--------|-----------|-------------|-----------|-------------|
| PI | 245,697 us | 122,873 us | 163,530 us | 81,749 us |
| PRIME | 164,844 us | 57,124 us | 109,707 us | 38,033 us |
| MANDEL | 31,352 us | 15,119 us | 20,875 us | 10,111 us |

## SC 222MHz基準の倍率 (PSP-1000)

| ベンチ | SC 222 | DUAL 222 | SC 333 | DUAL 333 |
|--------|--------|----------|--------|----------|
| PI | x1.00 | x2.00 | x1.50 | **x3.01** |
| PRIME | x1.00 | x2.89 | x1.50 | **x4.33** |
| MANDEL | x1.00 | x2.07 | x1.50 | **x3.09** |
| **平均** | x1.00 | x2.32 | x1.50 | **x3.48** |

## 機種間比較 (DUAL 333MHz)

| ベンチ | PSP-1000 | PSP-3000 | 差 |
|--------|----------|----------|-----|
| PI | 81,720 us | 81,749 us | 0.04% |
| PRIME | 38,010 us | 38,033 us | 0.06% |
| MANDEL | 10,108 us | 10,111 us | 0.03% |

**両機種で性能差なし** — 同一Allegrex 333MHzコア

## チェックサム (計算結果一致確認)
- PI: 3141595 (= pi * 1000000 ≈ 3.141595) — 全パターン・全機種一致
- PRIME: 9592 (10万以下の素数の個数) — 全パターン・全機種一致
- MANDEL: 173175 (全ピクセルのiter合計) — 全パターン・全機種一致

## クロック制御の知見

### scePowerSetClockFrequency
- 第1引数: CPU, 第2引数: RAM, 第3引数: BUS
- **上限333MHz** — 370指定はret=0を返すが実際は変わらない (サイレントクランプ)
- CFWで333固定にするとAPI無視される。「デフォルト」設定で初めてAPI有効
- バス周波数は111/166で切替確認済み

### タイマー
- sceKernelGetSystemTimeLow(): 0xBC600000 HWレジスタ, 1MHz固定, **クロック非依存**
- sceRtcGetCurrentTick(): 同上, 64bit版
- COP0 Count ($9): CPU_CLOCK/2でカウント, **クロック連動** — ユーザーモードでアクセス不可 (例外→フリーズ)

### OC (mcidclan psp-444mhz-plugin)
- PLLレジスタ直接制御でscePower上限を超える
- PSP-3000: 444MHz→即フリーズ, 407MHz→安定だが実効333MHz相当
- プラグインのPLL再初期化でベース周波数が低下し、OC分と相殺
- **結論: 実効的なOC効果は確認できなかった**

## PSP-3000 ME対応メモ
- kcall.prxをms0:/seplugins/に配置、game.txtに `ms0:/seplugins/kcall.prx 1` 追加で動作
- MECCのwitness word判定でT2テーブル(me_init_ret=2)が自動選択される
- PSP-1000はFATテーブル(me_init_ret=3)

## アプリ機能
- △: 全自動実行 (4パターン×3ベンチ)
- ○: 次の未完了ベンチ実行
- ×: 中断
- START: CSV保存
- **SELECT: USBオン/オフ切替** (XMBに戻らずPC接続可能)

## 要点 (記事用)
1. **ME並列で2〜3倍高速化** — 同一クロックでSC比2.0〜3.0倍
2. **222→333MHzクロックアップで1.5倍** — 理論値通り
3. **DUAL 333MHz = SC 222MHz比で最大4.3倍** (PRIME, 分割効率が良い)
4. **PSP-1000と3000で性能差ゼロ** — 同一CPUコア
5. **PSP-3000でもME使用可能** — kcall.prx seplugin必須
6. **333MHz超のOCは現行プラグインでは実効性なし**

## CSV保存先
- result_psp1000_final.csv — PSP-1000最終データ
- result_psp3000_final.csv — PSP-3000最終データ
