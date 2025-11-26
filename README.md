# 文件目录介绍
include_xxx和src_xxx为未完成的代码。
src目录中为原始可运行的代码。

# 环境配置
**操作系统：** Ubuntu:22.04<br><br>
**软件安装：**
- 系统包管理器安装<br>
```
  apt update \
  apt install -y vim curl wget\
  apt install -y git cmake build-essential g++  \
  apt install -y  hdf5-tools libhdf5-dev libvbz-hdf-plugin-dev
```
  
- 源码安装VBZ插件<br>
  由于系统包安装的插件ID和压缩文件中的插件ID不一致，无论h5dump或是编程接口，都无法正确解压数据。这里用源码安装的VBZ插件vbz_compress。安装步骤如下：<br>
```
  apt install -y libzstd-dev \
  git clone https://github.com/nanoporetech/vbz_compression.git \
  cd vbz_compression \
  git submodule update --init \
  mkdir build && cd build \
  cmake -D CMAKE_BUILD_TYPE=Release -D ENABLE_CONAN=OFF -D ENABLE_PERF_TESTING=OFF -D ENABLE_PYTHON=OFF .. \
  make -j
```
- 设置环境变量<br>
```
  echo 'export HDF5_PLUGIN_PATH=/usr/lib/x86_64-linux-gnu' >> ~/.bashrc
  source ~/.bashrc
```

**编译代码并运行:**
```
  mkdir build && cd build \
  cmake .. && make \
  mkdir output #测试结果输出到该文件夹
  ./hdf5_compress_test PBG08621_pass_6c7986d6_167483a9_0.hdf5 output/ #.hdf5文件路径需要修改为真实的路径
  column -s -t ',' ../output/hdf5_filter_results.csv | less -S #查看输出结果
```

**测试结果：**<br>
时间有限，暂时只有gzip和szip的测试结果。
| 压缩过滤器 | 参数配置   | 压缩级别 | 压缩比 | 压缩时间(ms) | 文件大小(MB) | 备注       |
|------------|------------|----------|--------|--------------|--------------|------------|
| None       | -          | -        | 1.0    | 318.519           | 363           | 基准（未压缩） |
| GZIP       | shuffle=1  | 1        | 0.608815   | 3579.65            | 221           | -          |
| GZIP       | shuffle=1  | 6        | 0.592287   | 5796.37            | 215           | -          |
| GZIP       | shuffle=1  | 9        | 0.589532   | 27897.9           | 214           | -          |
| SZIP       | -          | 1        | 0.512397   | 1399.48            | 185           | -          |
| LZ4       | -          | -        | 0.123967   | 412.151            | 45           | -          |

结论：从测试的这几组数据看，LZ4压缩算法完胜其他算法。压缩速度快，压缩效率高！




  
  




