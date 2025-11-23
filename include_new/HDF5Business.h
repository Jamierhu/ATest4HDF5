#include <iostream>
#include <H5Cpp.h>

// 要统计的压缩结果
class Result {
    std::string filter_name;  //压缩算法插件名称
    uint64_t file_mb; // 压缩后的文件大小（MB）
    double ratio; // 压缩文件大小 / 未压缩文件大小
    double compress_ms; // 压缩时间（毫秒）
};

class HDF5Business {
public:
    std::vector<Result> results; //记录压缩结果

    HDF5Business() = default;
    ~HDF5Business() = default;

    Result find_result(const std::string &filter_name); //根据压缩算法插件名称查找压缩结果
};


// 复制源文件中指定对象的所有属性到目标文件中的对应对象
void copy_attributes(hid_t src_loc, const std::string &name, hid_t dst_loc);

// 从指定路径读取数据集的原始字节数据
bool read_dataset_raw(H5::H5File &file, const std::string &path, std::vector<char> &outbuf,
                      hid_t &mem_type_id, std::vector<hsize_t> &dims_out, H5::DataType &cpp_dtype);
// 创建并写入数据集
bool create_and_write_dataset(H5::H5File &dst, const std::string &path,
                              hid_t mem_type_id, const std::vector<hsize_t> &dims,
                              const std::vector<char> &buf, const DSetCreatPropList &plist);

// 复制源文件中指定对象的所有属性到目标文件中的对应对象
bool copy_object_as_is(H5::H5File &src, H5::H5File &dst, const std::string &path);