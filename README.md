# 文件目录介绍
src目录中为原始可运行的代码。
include_xxx和src_xxx为未完成的代码，忽略之。

# 环境配置
**操作系统：** Ubuntu:22.04<br><br>
**软件安装：**
- 系统包管理器安装<br>
```
  apt update 
  apt install -y vim curl wget
  apt install -y git build-essential g++  
  apt install -y  hdf5-tools libhdf5-dev liblz4-dev
```
- 源码安装 cmake <br>
  源码安装hdf5需要至少3.26版本以上cmake，系统包安装的无法满足需求。
```
  git clone https://github.com/Kitware/CMake.git  
  cd CMake/
  ./bootstrap && make && make install
```
- 源码安装 hdf5 <br>
  hdf5_plugins使用需要源码安装hdf5，找到其需要的.cmake文件。
```
  git clone https://github.com/HDFGroup/hdf5.git
  mkdir build && cd build/
  cmake .. && make && make install
```
- 源码安装zstd<br>
  hdf5_plugins使用需要源码安装，找到其需要的.cmake文件。
```
  git clone https://github.com/facebook/zstd.git
  cd zstd/
  cmake -S . -B build-cmake
  cmake --build build-cmake
  make install
```
- 源码安装 hdf5_plugins <br>
  官方维护插件库，各种插件非常全面。但是lz4插件不支持设置压缩级别。
```
  export HDF5_PLUGIN_PATH=/usr/lib/x86_64-linux-gnu:/usr/local/lib/plugin #设置环境变量找到之前安装的依赖
  git clone https://github.com/HDFGroup/hdf5_plugins.git
  cd hdf5_plugins/
  mkdir build && cd build
  cmake .. && make && make install
```
**编译代码并运行代码:**
```
  git clone https://github.com/Jamierhu/ATest4HDF5.git
  cd ATest4HDF5
  mkdir build && cd build 
  cmake .. && make 
  mkdir output #测试结果输出到该文件夹
  ./hdf5_compress_test PBG08621_pass_6c7986d6_167483a9_0.hdf5 output/ #.hdf5文件路径需要修改为真实的路径
  column -s -t ',' ../output/hdf5_filter_results.csv | less -S #查看输出结果
```

**测试结果：**<br>

| 压缩过滤器 | 参数配置   | 压缩级别 | 压缩比 | 压缩时间(ms) | 文件大小(MB) | 备注       |
|------------|------------|----------|--------|--------------|--------------|------------|
| None       | -          | -        | 1.0    | 318.519           | 363           | 基准（未压缩） |
| GZIP       | shuffle=1  | 1        | 0.608815   | 3579.65            | 221           | -          |
| GZIP       | shuffle=1  | 6        | 0.592287   | 5796.37            | 215           | -          |
| GZIP       | shuffle=1  | 9        | 0.589532   | 27897.9           | 214           | -          |
| SZIP       | -          | 1        | 0.512397   | 1399.48            | 185           | -          |
| LZ4       | -          | -        | 0.123967   | 412.151            | 45           | -          |
| Zstd       | -          | 1        | 0.123967   | 835.203            | 264           | -          |
| Zstd       | -          | 11        | 0.123967   | 9357.04            | 241           | -          |
| Zstd       | -          | 22        | 0.123967   | 24300.6            | 239           | -          |


结论：从测试的这几组数据看，LZ4压缩算法完胜其他算法。压缩速度快，压缩效率高！




  
  




