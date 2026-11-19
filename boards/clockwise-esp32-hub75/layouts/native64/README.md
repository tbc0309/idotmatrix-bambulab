# 原生 64×64 布局 / Native 64×64 layout

在仓库根目录指定独立构建目录，选择原生 64×64 布局。

Build from the repository root with a separate build directory to select the native 64×64 layout.

```sh
idf.py -B build-clockwise-native64 -D MATRIX_BOARD=clockwise-esp32-hub75 -D CLOCKWISE_SCALE_LAYOUT=OFF build
```
