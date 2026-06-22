# DT-CWPT

JUCE 8.x 向け Dual-Tree Complex Wavelet Packet Transform ライブラリ

[English](README.md) | 日本語

---

## 概要

本ライブラリは、Dual-Tree Complex Wavelet Packet Transform（Bayram & Selesnick, 2008）を JUCE モジュールとして実装したものです。設定可能なツリートポロジーにより、デュアルツリー複素ウェーブレットフィルタバンクを用いてオーディオ信号のサブバンド分解を行い、完全再構成で出力を復元します。コールバックインターフェースにより、解析と合成の間で複素サブバンドに対するユーザー定義の処理が可能です。

昨今の楽曲制作ではボコーダーやケプストラムモーフィングといった2音を合成するエフェクトや、STFTを用いて周波数領域で直接音声を操作するエフェクトなどがサウンドデザインに広く用いられています。しかし、STFTの不確定性原理により、低域の高い周波数分解能と高域の高い時間分解能が両立できず、トランジェントの精度と周波数解像度のどちらかを犠牲にする必要があります。そこで、多重解像度解析である複素ウェーブレットパケット変換を用いることで、低域の高い周波数分解能と高域の高い時間分解能した周波数軸での処理を実現します。また、複素ウェーブレットパケット変換の振幅の時間変化はその帯域の振幅包絡の情報を、位相の時間変化はその帯域の周波数の情報を持ちます。そのため、一方の音声のエンベロープのみ、あるいは音色のみをもう一方の音声に合成させる処理に適しており、より高い精度で2音を合成するエフェクトが実現できます。

---

## 動作要件

- JUCE: 8.0.12 で動作確認済み（6.x 以降でも動作する可能性がありますが保証はされません）
- C++: C++20 以降（C++20 で動作確認済み）

---

## 特徴

- 設定可能なツリートポロジー: フルパケット、ウェーブレットツリー、または任意の混合深度二分木
- 任意の有効なトポロジーに対する完全再構成
- `BandProcessor` インターフェースによるプラグイン可能なサブバンド処理（バンドごとまたはクロスバンド）
- Re/Im から Magnitude/Phase への変換（インプレース対応）
- パラレル解析のためのサイドチェーン入力
- 自動バンド間遅延補償によるサンプル精度のステートフルフィルタリング

---

## プロジェクト構成

```
dtcwpt/                  <- JUCE モジュールルート
├── dtcwpt.h             <- マスターヘッダー + モジュール宣言
├── dtcwpt.cpp           <- Unity ビルドファイル
├── dtcwpt_processor.h/.ipp
├── dtcwpt_analysis_node.h/.ipp
├── dtcwpt_synthesis_node.h/.ipp
├── dtcwpt_stateful_filter.h/.ipp
├── dtcwpt_delay_buffer.h/.ipp
├── dtcwpt_topology_planner.h/.ipp
├── dtcwpt_band_processor.h/.ipp
├── dtcwpt_complex_utils.h/.ipp
├── dtcwpt_filter_structs.h
└── dtcwpt_filter_coeffs.h
tests/                   <- ユニットテスト（オプション）
```

---

## 基本的な使い方

### 最小構成（パススルー）

```cpp
#include <dtcwpt/dtcwpt.h>

// プロセッサを作成
dtcwpt::DTCWPTProcessor processor;

// トポロジーを設定
dtcwpt::TopologyConfig config;
config.destinations = {"LLL", "LLH", "LHL", "LHH", "HLL", "HLH", "HHL", "HHH"};
config.maxDepth = 12; // 任意指定。既定値はサポート上限

// 初期化
processor.prepareToPlay(sampleRate, maxBlockSize, config, numChannels);

// ホスト補償用のレイテンシーを取得
int latency = processor.getLatency();

// オーディオブロックを処理（解析 -> 合成パススルー）
processor.processBlock(audioBuffer);
```

### バンド処理あり

```cpp
// バンドごとのプロセッサを登録（例: 高周波バンドを減衰）
processor.setBandProcessor(dtcwpt::makeLambdaProcessor(
    [](int bandIndex, double* re, double* im, size_t numSamples) {
        if (bandIndex >= 4) {
            for (size_t i = 0; i < numSamples; ++i) {
                re[i] *= 0.5;
                im[i] *= 0.5;
            }
        }
    }
));

// processBlock() が解析と合成の間でバンドプロセッサを適用するようになります
processor.processBlock(audioBuffer);
```

### Magnitude/Phase 処理あり

```cpp
processor.setBandProcessor(dtcwpt::makeLambdaProcessor(
    [](int bandIndex, double* re, double* im, size_t n) {
        // Magnitude/Phase に変換（インプレース: re が mag に、im が phase になる）
        dtcwpt::complexToMagPhase(re, im, re, im, n);

        // Magnitude を変更（例: スペクトルゲーティング）
        for (size_t i = 0; i < n; ++i) {
            if (re[i] < 0.01) re[i] = 0.0;  // re[i] はここでは magnitude
        }

        // Re/Im に戻す（インプレース: mag が re に、phase が im になる）
        dtcwpt::magPhaseToComplex(re, im, re, im, n);
    }
));
```

---

## 統合方法

### 方法 1: Projucer

`dtcwpt/` フォルダを Projucer プロジェクトの「Modules」セクションにドラッグしてください。モジュールが自動的に設定されます。

### 方法 2: CMake

`CMakeLists.txt` で JUCE モジュールとしてライブラリを追加します:

```cmake
# dtcwpt モジュールを登録
juce_add_module("path/to/dtcwpt")

# ターゲットにリンク
target_link_libraries(MyTarget PRIVATE dtcwpt)
```

### ヘッダーのインクルード

```cpp
#include <dtcwpt/dtcwpt.h>
```

---

## テスト

`tests/` ディレクトリにユニットテストが含まれています。

### テストのビルド

前提条件: JUCE が環境内で利用可能であること。

ビルドコマンド:
```bash
cmake -B build -S tests
cmake --build build --config Release
ctest --test-dir build -C Release --output-on-failure
```

ビルドシステムは以下の順序で JUCE を自動的に検索します:
1. システムにインストールされた JUCE（CMake の `find_package` 経由）
2. ローカルの `../JUCE` ディレクトリ（例: git サブモジュール）
3. CMake FetchContent による自動ダウンロード（上記のいずれも見つからない場合）

---

## 重要な制約

### 深度制限

- サポートされる最大ツリー深度: 12（フル深度で最大 4096 リーフバンド）
- `TopologyConfig::maxDepth` の既定値はこのサポート上限です
- `prepareToPlay()` は最も深いデスティネーションパス長 (`actualDepth`) が `maxDepth` を超える構成を拒否します

### トポロジー（デスティネーション）ルール

`TopologyConfig::destinations` は二分ウェーブレットパケットツリーのリーフノードを定義します。
各デスティネーションは `'L'`（ローパス）と `'H'`（ハイパス）の文字で構成されるパス文字列で、文字列の長さはノードの深度に等しくなります。

デスティネーションは、すべてのノードが 0 個または 2 個の子を持つ有効な二分木の完全なリーフセットを形成しなければなりません。
`prepareToPlay()` は最も深いデスティネーションのパス長を `actualDepth` として計算し、`actualDepth <= maxDepth` を要求します。

つまり:

- ノードが分割される（子を持つ）場合、L の子と H の子の両方がツリーに存在しなければなりません
- リーフノード（デスティネーション）は子を持ちません
- `prepareToPlay()` 実行時に、パスの長さは `maxDepth` 文字を超えてはなりません

#### パス文字列のエンコーディング

| パス | 意味 | 深度 | ノードインデックス |
|------|------|------|-------------------|
| `"L"` | ルート -> Low | 1 | 2 |
| `"H"` | ルート -> High | 1 | 3 |
| `"LL"` | ルート -> Low -> Low | 2 | 4 |
| `"LH"` | ルート -> Low -> High | 2 | 5 |
| `"HLL"` | ルート -> High -> Low -> Low | 3 | 12 |

#### 有効なトポロジーの例

例 1: 深度 2 のフルパケット -- すべてのリーフが同じ深度。

```
         (root)
        /      \
       L        H           深度 1
      / \      / \
    LL   LH  HL   HH       深度 2 (すべてリーフ)

destinations = {"LL", "LH", "HL", "HH"}    有効
```

すべての内部ノード（root、L、H）が正確に 2 つの子を持ちます。4 つのリーフがすべてリストされています。

例 2: 混合深度ツリー -- 異なる深度にリーフが存在。

```
         (root)
        /      \
       L        H           "L" はリーフ（深度 1）
               / \
             HL   HH        "HL","HH" はリーフ（深度 2）

destinations = {"L", "HL", "HH"}    有効
```

`L` はリーフ（子なし）。`H` は内部ノードで、`HL` と `HH` の両方の子を持ちます。

例 3: 深度 3 のウェーブレットツリー -- L ブランチのみが完全に分解。

```
              (root)
             /      \
            L        H         "H" はリーフ（深度 1）
           / \
         LL   LH               "LH" はリーフ（深度 2）
        / \
      LLL  LLH                 "LLL","LLH" はリーフ（深度 3）

destinations = {"LLL", "LLH", "LH", "H"}    有効
```

#### 無効なトポロジーの例

無効 1: 兄弟ノードの欠落 -- ノード H が分割されているが、HL のみ存在（HH が欠落）。

```
         (root)
        /      \
       L        H
               /
             HL        <-- HH はどこに?

destinations = {"L", "HL"}    無効 -- H は 1 つの子しか持たない
```

無効 2: パスの重複 -- `"L"` がリーフとしてリストされているが、`"LL"` は L が内部ノードであることを意味する。

```
destinations = {"L", "LL", "LH", "H"}    無効 -- L はリーフと親の両方にはなれない
```

無効 3: 不完全なリーフ -- 内部ノード L は子 LL と LH を持つが、LL のみがリストされている。

```
destinations = {"LL", "H"}    無効 -- LH が欠落（L は両方の子を持たなければならない）
```

`prepareToPlay()` は、空のデスティネーションリスト、`1..12` の範囲外の `maxDepth`、および最も深いデスティネーションパス長が設定済み `maxDepth` を超えるトポロジーを拒否します。これらの深度関連チェック以外の形状バリデーションは引き続き最小限のため、不正なツリー構造は未定義動作につながる可能性があります。`prepareToPlay()` に渡す前にトポロジーを検証してください。

### 倍精度浮動小数点

本ライブラリは全体を通じて `juce::AudioBuffer<double>` を使用します。JUCE の標準プラグイン API は `float` バッファを使用します。プラグインの `processBlock()` で `float` と `double` の間で変換する必要があります:

```cpp
// float -> double（処理前）
for (int ch = 0; ch < numChannels; ++ch) {
    const float* src = floatBuffer.getReadPointer(ch);
    double* dst = doubleBuffer.getWritePointer(ch);
    for (int i = 0; i < numSamples; ++i)
        dst[i] = static_cast<double>(src[i]);
}

processor.processBlock(doubleBuffer);

// double -> float（処理後）
for (int ch = 0; ch < numChannels; ++ch) {
    const double* src = doubleBuffer.getReadPointer(ch);
    float* dst = floatBuffer.getWritePointer(ch);
    for (int i = 0; i < numSamples; ++i)
        dst[i] = static_cast<float>(src[i]);
}
```

`doubleBuffer` は `prepareToPlay()` で事前に確保し、オーディオスレッドでのヒープ割り当てを避けてください。

---

## バンド処理

### パイプライン概要

DT-CWPT プロセッサは固定の 4 ステージパイプラインに従います:

```
入力 -> [解析] -> [遅延補償] -> [バンド処理] -> [合成] -> 出力
```

| ステージ | 説明 |
| -------- | ---- |
| 解析 | 各デスティネーションでデュアルツリーフィルタバンクを通じて入力をサブバンドに分解 |
| 遅延補償 | クロスバンド処理前にサブバンドの位相を揃える（自動） |
| バンド処理 | `BandProcessor` によるユーザー定義のサブバンド処理（オプション） |
| 合成 | 逆変換により処理済みサブバンドから出力信号を再構成 |

`BandProcessor` が登録されていない場合、パイプラインはパススルー（完全再構成）として動作します。

### BandProcessor インターフェース

解析と合成の間でサブバンドを処理するために `BandProcessor` インターフェースを実装します:

```cpp
class BandProcessor {
public:
    virtual void prepare(double sampleRate, int maxSamplesPerBand,
                         int numBands, int numChannels) = 0;
    virtual void processAllBands(BandData& data);   // クロスバンドアルゴリズム用にオーバーライド
    virtual void processBand(int bandIndex, double* re, double* im,
                             size_t numSamples);     // バンドごとの処理用にオーバーライド
    virtual void reset() = 0;
};
```

- `prepare()` は `DTCWPTProcessor::prepareToPlay()` 実行時、または prepare 後に `setBandProcessor()` が呼ばれたときに呼び出されます。
- `processAllBands()` のデフォルト実装は、すべてのチャンネルとバンドに対して反復し、各バンドで `processBand()` を呼び出します。クロスバンドまたはクロスチャンネルアルゴリズム（例: スペクトルモーフィング）にはこれをオーバーライドしてください。
- `processBand()` のデフォルト実装はノーオペレーション（パススルー）です。単純なバンドごとの処理にはオーバーライドしてください。
- `reset()` は再生が再開されるときに呼び出されます。

すべてのメソッドはオーディオスレッドで呼び出され、リアルタイムセーフでなければなりません（アロケーション、ロック、システムコール不可）。

バンドプロセッサの登録:

```cpp
processor.setBandProcessor(std::move(myBandProcessor));
```

### makeLambdaProcessor の使用

単純なバンドごとの処理には、サブクラス化の代わりにコンビニエンスファクトリを使用できます:

```cpp
// BandProcessorFunc = std::function<void(int bandIndex, double* re, double* im, size_t numSamples)>
auto bp = dtcwpt::makeLambdaProcessor(
    [](int bandIndex, double* re, double* im, size_t numSamples) {
        // バンドごとの処理をここに記述
    }
);
processor.setBandProcessor(std::move(bp));
```

### 複素ユーティリティ（Re/Im <-> Mag/Phase）

本ライブラリは、直交座標系（Re/Im）と極座標系（Magnitude/Phase）間の変換のためのユーティリティ関数を提供します。これらはバンドプロセッサ内でのマグニチュードベースのスペクトル処理に有用です。

```cpp
// Re/Im -> Magnitude/Phase
//   mag[i] = sqrt(re[i]^2 + im[i]^2)
//   phase[i] = atan2(im[i], re[i])
dtcwpt::complexToMagPhase(re, im, mag, phase, n);

// Magnitude/Phase -> Re/Im
//   re[i] = mag[i] * cos(phase[i])
//   im[i] = mag[i] * sin(phase[i])
dtcwpt::magPhaseToComplex(mag, phase, re, im, n);
```

インプレース操作がサポートされています。入力と出力に同じポインタを渡すことができ（例: `re == mag` かつ `im == phase`）、オーディオスレッドでの一時バッファの必要性を回避します:

```cpp
// インプレース: re が magnitude に、im が phase になる
dtcwpt::complexToMagPhase(re, im, re, im, n);
// ... magnitude/phase を変更 ...
// インプレース: magnitude が re に、phase が im になる
dtcwpt::magPhaseToComplex(re, im, re, im, n);
```

### サイドチェーンサポート

パラレル CWPT 解析のためにサイドチェーン信号を入力できます（例: 2 つの信号間のスペクトルモーフィング）:

```cpp
// 同じブロックの processBlock() の前に呼び出す必要があります
processor.processSidechain(sidechainBuffer);
processor.processBlock(mainBuffer);
```

`BandProcessor::processAllBands()` 内で `BandData` 経由でサイドチェーンデータにアクセスします:

```cpp
void processAllBands(BandData& data) override {
    if (data.hasSidechain) {
        for (int ch = 0; ch < data.numChannels; ++ch) {
            for (int b = 0; b < data.numBands; ++b) {
                auto& main = data.bands[ch][b];
                auto& sc   = data.sidechainBands[ch][b];
                // sc.re, sc.im を main.re, main.im と併せて使用
            }
        }
    }
}
```

サイドチェーンのチャンネル数はメイン入力に合わせて自動的に調整されます（最後のチャンネルの複製または切り捨て）。

---

## API リファレンス

### DTCWPTProcessor

```cpp
void prepareToPlay(double sampleRate, int maxBlockSize,
                   const TopologyConfig& config, int channelNum);
```

指定されたパラメータでプロセッサを初期化します。処理の前に呼び出す必要があります。

---

```cpp
void processBlock(juce::AudioBuffer<double>& buffer);
```

オーディオブロックを処理します。任意のバッファサイズを受け付け、内部 FIFO がブロックサイズの適応を自動的に処理します。

---

```cpp
int getLatency() const;
```

サンプル数での処理レイテンシーを返します。ホストのレイテンシー補償に使用してください。

---

```cpp
void setBandProcessor(std::unique_ptr<BandProcessor> processor);
```

バンドプロセッサを登録します（所有権を取得）。`prepareToPlay()` が既に呼び出されている場合、プロセッサの `prepare()` を即座に呼び出します。オーディオが動作していないときに呼び出す必要があります。

---

```cpp
void processSidechain(juce::AudioBuffer<double>& sidechainBuffer);
```

パラレル CWPT 解析のためにサイドチェーンオーディオを入力します。メインの `processBlock()` と同じサンプル数で呼び出す必要があり、同じブロックの `processBlock()` の前に呼び出す必要があります。

---

### TopologyConfig

```cpp
struct TopologyConfig {
    std::vector<std::string> destinations;  // パス文字列（例: "LLL", "LH"）
    int maxDepth = 12;                      // 既定でサポート上限の最大ツリー深度（1-12）
};
```

ウェーブレットパケットツリーの構造を定義します。`maxDepth` の既定値はサポート上限であり、`prepareToPlay()` は `actualDepth <= maxDepth` を要求します。有効性の制約については[トポロジー（デスティネーション）ルール](#トポロジーデスティネーションルール)を参照してください。

---

### BandProcessor

```cpp
class BandProcessor {
public:
    virtual void prepare(double sampleRate, int maxSamplesPerBand,
                         int numBands, int numChannels) = 0;
    virtual void processAllBands(BandData& data);
    virtual void processBand(int bandIndex, double* re, double* im,
                             size_t numSamples);
    virtual void reset() = 0;
};
```

ユーザー定義サブバンド処理のための抽象インターフェース。使用方法の詳細は[バンド処理](#バンド処理)を参照してください。

---

### BandData

```cpp
struct BandData {
    std::vector<std::vector<ChannelBandView>> bands;          // bands[ch][bandIndex]
    std::vector<std::vector<ChannelBandView>> sidechainBands; // サイドチェーン（非アクティブの場合は空）
    int numChannels;
    int numBands;
    bool hasSidechain;
    const std::vector<int>* destinationIds;  // バンド順のノード ID
};
```

`processAllBands()` に渡される全バンド・全チャンネルのデータコンテナ。バンドインデックスは `TopologyConfig::destinations` の順序に対応します。プロセッサの内部バッファへのゼロコピーポインタを使用します。

---

### ChannelBandView

```cpp
struct ChannelBandView {
    double* re;           // サブバンドデータの実部へのポインタ（変更可能）
    double* im;           // サブバンドデータの虚部へのポインタ（変更可能）
    size_t numSamples;    // このバンドのサブバンドサンプル数
};
```

単一チャンネルのバンドごとのデータビュー。サブバンドの Re/Im データへの変更可能なアクセスを提供します。

---

### makeLambdaProcessor

```cpp
using BandProcessorFunc = std::function<void(int bandIndex, double* re,
                                             double* im, size_t numSamples)>;

std::unique_ptr<BandProcessor> makeLambdaProcessor(BandProcessorFunc func);
```

ラムダまたは関数オブジェクトから `BandProcessor` を作成するコンビニエンスファクトリ。返されるプロセッサは `processBand()` を提供された関数に委譲します。`prepare()` と `reset()` はノーオペレーションです。

---

### 複素ユーティリティ

```cpp
void complexToMagPhase(const double* re, const double* im,
                       double* mag, double* phase, size_t n);

void magPhaseToComplex(const double* mag, const double* phase,
                       double* re, double* im, size_t n);
```

直交座標系（Re/Im）と極座標系（Magnitude/Phase）間の変換。インプレース操作がサポートされています（`re == mag` かつ `im == phase` は有効）。

---

## 技術的詳細

### 背景

標準的な離散ウェーブレット変換（DWT）は、反復 2 チャンネルフィルタバンク（ローパス/ハイパス解析、2 によるダウンサンプリング、および対応する合成）を通じて信号を分解します。DWT はこれをローパスブランチにのみ適用し、対数的な周波数タイリングを生成します。しかし、デシメーション付き DWT はシフト変動的であり、入力の小さな時間シフトが係数の大きな変化を引き起こします。

Dual-Tree Complex Wavelet Transform（DT-CWT）[1] は、近似ヒルベルト対を形成するウェーブレットを持つ 2 つの並列フィルタバンクを実行することでこれを軽減します。得られる複素係数はほぼシフト不変のマグニチュードとクリーンに分離された位相を持ちます。ウェーブレットパケット拡張（DT-CWPT）[2] はツリー構造を一般化し、ローパスとハイパスの両方のブランチをさらに分解できるようにすることで、DT-CWT の固定対数レイアウトを超えた任意の周波数タイリングを可能にします。

### フィルタ係数

本実装は 2 セットのフィルタを使用します:

| レベル | フィルタ | タップ数（lo / hi） |
|--------|----------|---------------------|
| 1 | CDF 9/7 双直交 | 10 / 8 |
| >= 2 | Kingsbury Q-shift | 14 / 14 |

2 つのツリーのフィルタセット間のハーフサンプル遅延が、ヒルベルト対関係に必要な近似 90 度位相シフトを生成します。

### 完全再構成

完全再構成は、任意の有効な二分木トポロジー（すべての内部ノードが正確に 2 つの子を持つ）に対して成立します。PR フィルタバンクの条件は:

$$H_0(z)G_0(z) + H_1(z)G_1(z) = 2z^{-l}$$

$$H_0(-z)G_0(z) + H_1(-z)G_1(z) = 0$$

### 参考文献

1. Selesnick, I. W., Baraniuk, R. G., & Kingsbury, N. G. (2005). The dual-tree complex wavelet transform. *IEEE Signal Processing Magazine*, 22(6), 123-151. https://doi.org/10.1109/MSP.2005.1550194

2. Bayram, I., & Selesnick, I. W. (2008). On the dual-tree complex wavelet packet and M-band transforms. *IEEE Transactions on Signal Processing*, 56(6), 2298-2310. https://doi.org/10.1109/TSP.2007.916129

---

## ライセンス

MIT License - 詳細は [LICENSE](LICENSE) ファイルを参照してください。
