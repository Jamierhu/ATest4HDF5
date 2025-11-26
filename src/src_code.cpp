#include <H5Cpp.h>
#include <hdf5.h> 
#include <iostream>
#include <vector>
#include <string>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <regex>
#include <sstream>
#include "hdf5/serial/hdf5.h"
//#include <vbz-compression/vbz.h>
//#include <vbz-compression/vbz_plugin.h>

#define H5Z_FILTER_LZ4 32004
#define H5Z_FILTER_ZSTD 32015

namespace fs = std::filesystem;
using namespace H5;

struct Result {
    std::string filter_name;
    uint64_t file_mb;
    double ratio; // compressed / baseline
    double compress_ms;
};

// 判断是都要解压的数据集
bool is_target_dataset(const std::string &fullpath, const std::string &dset_name) {
    if (!(dset_name == "Raw" || dset_name == "Signal")) return false;

    std::regex re("(^|/)(read_[^/]+)(/|$)");
    return std::regex_search(fullpath, re);
}

// 复制属性从src_loc/name到dst_loc
void copy_attributes(hid_t src_loc, const std::string &name, hid_t dst_loc) {
    hid_t obj = H5Oopen(src_loc, name.c_str(), H5P_DEFAULT);
    if (obj < 0) return;
    H5O_info_t oinfo;
    if (H5Oget_info(obj, &oinfo) < 0) { H5Oclose(obj); return; }
    // 遍历属性
    int nattrs = H5Aget_num_attrs(obj); // C API
    for (int i = 0; i < nattrs; ++i) {
        hid_t attr = H5Aopen_by_idx(obj, ".", H5_INDEX_NAME, H5_ITER_INC, (hsize_t)i, H5P_DEFAULT, H5P_DEFAULT);
        if (attr < 0) continue;
        // 获取属性名称
        ssize_t name_len = H5Aget_name(attr, 0, nullptr);
        std::string aname(name_len + 1, '\0');
        H5Aget_name(attr, name_len + 1, &aname[0]);
        aname.resize(name_len);
        // 获取属性类型和空间
        hid_t atype = H5Aget_type(attr);
        hid_t aspace = H5Aget_space(attr);
        // 读取原始属性数据
        hsize_t asize = H5Tget_size(atype);
        // 创建目标对象
        hid_t dst_obj = H5Oopen(dst_loc, name.c_str(), H5P_DEFAULT);
        if (dst_obj < 0) { H5Tclose(atype); H5Sclose(aspace); H5Aclose(attr); continue; }
        hid_t dst_attr = H5Acreate2(dst_obj, aname.c_str(), atype, aspace, H5P_DEFAULT, H5P_DEFAULT);
        if (dst_attr >= 0) {
            // 读取属性数据并写入目标属性
            hssize_t nelmts = H5Sget_simple_extent_npoints(aspace);
            size_t tsize = H5Tget_size(atype);
            std::vector<char> buf(tsize * nelmts);
            if (H5Aread(attr, atype, buf.data()) >= 0) {
                H5Awrite(dst_attr, atype, buf.data());
            }
            H5Aclose(dst_attr);
        }
        H5Oclose(dst_obj);
        H5Tclose(atype);
        H5Sclose(aspace);
        H5Aclose(attr);
    }
    H5Oclose(obj);
}

// 读取数据集的原始字节
bool read_dataset_raw(H5::H5File &file, const std::string &path, std::vector<char> &outbuf,
                      hid_t &mem_type_id, std::vector<hsize_t> &dims_out, H5::DataType &cpp_dtype) {
    try {
        DataSet ds = file.openDataSet(path);
        DataSpace space = ds.getSpace();
        int rank = space.getSimpleExtentNdims();
        dims_out.resize(rank);
        space.getSimpleExtentDims(dims_out.data(), nullptr);

        //获取本机数据类型
        cpp_dtype = ds.getDataType();
        hid_t native_tid = H5Tget_native_type(cpp_dtype.getId(), H5T_DIR_DEFAULT);
        mem_type_id = native_tid;

        // 计算缓冲区大小
        hsize_t total = 1;
        for (auto d : dims_out) total *= d;
        size_t type_size = H5Tget_size(native_tid);
        outbuf.resize(static_cast<size_t>(total) * type_size);

        herr_t err = H5Dread(ds.getId(), native_tid, H5S_ALL, H5S_ALL, H5P_DEFAULT, outbuf.data());
        if (err < 0) {
            std::cerr << "Error reading dataset: " << path << "\n";
            H5Tclose(native_tid);
            return false;
        }
        return true;
    } catch (...) {
        std::cerr << "Exception reading dataset: " << path << "\n";
        return false;
    }
}

//创建并写入数据集
bool create_and_write_dataset(H5::H5File &dst, const std::string &path,
                              hid_t mem_type_id, const std::vector<hsize_t> &dims,
                              const std::vector<char> &buf, const DSetCreatPropList &plist) {
    try {
        // 检查组是否存在，若不存在则创建
        std::string p = path;
        if (p.front() == '/') p.erase(0,1);
        size_t pos = 0;
        std::string cur = "";
        while (true) {
            size_t slash = p.find('/', pos);
            std::string token = (slash==std::string::npos) ? p.substr(pos) : p.substr(pos, slash-pos);
            pos = (slash==std::string::npos) ? std::string::npos : slash+1;
            if (pos==std::string::npos) {
                break;
            }
            cur += "/" + token;
            try {
                Group g = dst.openGroup(cur);
            } catch(...) {
                dst.createGroup(cur);
            }
            if (pos==std::string::npos) break;
        }
        // 创建数据集
        DataSpace space(static_cast<int>(dims.size()), dims.data());
        DataType dtype(mem_type_id);
        DataSet ds = dst.createDataSet(path, dtype, space, plist);
        herr_t err = H5Dwrite(ds.getId(), mem_type_id, H5S_ALL, H5S_ALL, H5P_DEFAULT, buf.data());
        return err >= 0;
    } catch (...) {
        return false;
    }
}

// 非解压对象,直接复制保持不变
bool copy_object_as_is(H5::H5File &src, H5::H5File &dst, const std::string &path) {
    try {
        // 检查对象类型
        H5O_info_t oinfo;
        herr_t e = H5Oget_info_by_name(src.getId(), path.c_str(), &oinfo, H5P_DEFAULT);
        if (e < 0) return false;
        if (oinfo.type == H5O_TYPE_DATASET) {
            std::vector<char> buf;
            hid_t memtid;
            std::vector<hsize_t> dims;
            DataType cppdtype;
            if (!read_dataset_raw(src, path, buf, memtid, dims, cppdtype)) return false;
            DSetCreatPropList plist; // default: no compression, contiguous or default
            bool ok = create_and_write_dataset(dst, path, memtid, dims, buf, plist);
            H5Tclose(memtid);
            return ok;
        } else if (oinfo.type == H5O_TYPE_GROUP) {
            // 创建组并复制属性
            try {
                dst.createGroup(path);
            } catch(...) {}
            hid_t src_loc = src.getId();
            hid_t dst_loc = dst.getId();
            copy_attributes(src_loc, path.c_str(), dst_loc);
            // 递归复制子对象
            Group gsrc = src.openGroup(path);
            hsize_t n = gsrc.getNumObjs();
            for (hsize_t i = 0; i < n; ++i) {
                std::string name = gsrc.getObjnameByIdx(i);
                std::string child_path = path;
                if (child_path.back() != '/') child_path += "/";
                child_path += name;
                copy_object_as_is(src, dst, child_path);
            }
            return true;
        } else {
            return true;
        }
    } catch (...) {
        return false;
    }
}

int main(int argc, char **argv) {
    if (argc < 3) {
        std::cout << "Usage: " << argv[0] << " <source.h5> <out-dir>\n";
        std::cout << "Example: " << argv[0] << " data.h5 out\n";
        return 1;
    }
    std::string src_path = argv[1];
    fs::path outdir = argv[2];
    fs::create_directories(outdir);

    // 插件过滤器需要在运行前注册
    struct FilterSpec {
        std::string name;
        std::function<void(DSetCreatPropList&)> apply;
        bool requires_avail; // 是否需要检测可用性
        unsigned int check_id; // 插件过滤器ID
    };
    std::vector<FilterSpec> specs;

    // 基线文件
    specs.push_back({"baseline_none", [](DSetCreatPropList &p){ /* no change */ }, false, 0});

    // shuffle + gzip levels 1,6,9
    unsigned int gzip_cp_levs[3] = {1,6,9};
    for (unsigned int lev : gzip_cp_levs) {
        std::string fname = "shuffle_gzip_lvl" + std::to_string(lev);
        specs.push_back({fname, [lev](DSetCreatPropList &p){ p.setShuffle(); p.setDeflate(lev); }, false, H5Z_FILTER_DEFLATE});
    }
    // szip
#ifdef H5Z_FILTER_SZIP
    specs.push_back({"szip", [](DSetCreatPropList &p){
    }, true, H5Z_FILTER_SZIP});
#else
    specs.push_back({"szip", [](DSetCreatPropList &p){}, true, H5Z_FILTER_SZIP});
#endif

    // vbz levels 1,11,22
    /*unsigned int vbz_cp_levs[3] = {1,11,22};
    for (unsigned int lev : vbz_cp_levs) {
        std::string fname = "shuffle_vbz_lvl" + std::to_string(lev);
        unsigned int cd_vals[4] = {0, lev, 1, 1};
        specs.push_back(
            {fname,
            [cd_vals](DSetCreatPropList &p){
                p.setFilter(FILTER_VBZ_ID, H5Z_FLAG_MANDATORY , 4, cd_vals);
            },
            true,      
            FILTER_VBZ_ID
        });
    }*/

    // lz4 (no level setting)
    specs.push_back({"lz4", [](DSetCreatPropList &p){
        p.setFilter(H5Z_FILTER_LZ4 , H5Z_FLAG_MANDATORY, 0, nullptr);
    }, true, H5Z_FILTER_LZ4 });

    // zstd 
    unsigned int levels[3] = {1, 11, 22};
    for (unsigned int lev : levels) {
        std::string fname = "zstd_lvl" + std::to_string(lev);
        specs.push_back({fname, [lev](DSetCreatPropList &p){
            p.setFilter(H5Z_FILTER_ZSTD, H5Z_FLAG_MANDATORY , 1,&lev);
        }, true, H5Z_FILTER_ZSTD });
    }

    // 打开源文件
    H5::Exception::dontPrint();
    H5::H5File src;
    try {
        src = H5File(src_path, H5F_ACC_RDONLY);
    } catch (const FileIException &e) {
        std::cerr << "Failed to open source file: " << src_path << "\n";
        return 2;
    }

    // 创建输出目录
    fs::path baseline_file = outdir / "baseline_none.h5";
    auto run_one = [&](const FilterSpec &spec) -> Result {
        std::string fname = spec.name + ".h5";
        fs::path outpath = outdir / fname;
        // 创建输出文件，若存在则删除
        if (fs::exists(outpath)) fs::remove(outpath);
        H5::H5File dst;
        try {
            dst = H5File(outpath.string(), H5F_ACC_TRUNC);
        } catch (...) {
            std::cerr << "Failed to create " << outpath << "\n";
            return Result{spec.name,0,0,0};
        }

        // 递归遍历源文件对象，复制数据集和组
        double compress_ms = 0.0; //累计压缩时间
        std::function<void(H5::Group, H5::Group, const std::string&)> recurse;
        recurse = [&](H5::Group gsrc, H5::Group gdst, const std::string &gpath) {
            hsize_t n = gsrc.getNumObjs();
            for (hsize_t i = 0; i < n; ++i) {
                std::string name = gsrc.getObjnameByIdx(i);
                H5G_obj_t type = gsrc.getObjTypeByIdx(i);
                std::string child_src_path = gpath;
                if (child_src_path == "/") child_src_path = "/" + name;
                else child_src_path = gpath + "/" + name;

                if (type == H5G_GROUP) {
                    // 创建目的组
                    try { gdst.createGroup(name); } catch(...) {}
                    // 复制属性
                    hid_t src_loc = gsrc.getId();
                    hid_t dst_loc = gdst.getId();
                    copy_attributes(src_loc, name.c_str(), dst_loc);
                    Group ngsrc = gsrc.openGroup(name);
                    Group ngdst = gdst.openGroup(name);
                    recurse(ngsrc, ngdst, child_src_path);
                } else if (type == H5G_DATASET) {
                    // 检查是否为目标数据集
                    bool is_target = is_target_dataset(child_src_path, name);
                    // 读取源数据集原始数据
                    std::vector<char> buf;
                    hid_t memtid = -1;
                    std::vector<hsize_t> dims;
                    DataType cppdtype;
                    bool ok = read_dataset_raw(src, child_src_path, buf, memtid, dims, cppdtype);
                    if (!ok) {
                        std::cerr << "Warning: failed read dataset " << child_src_path << "\n";
                        continue;
                    }

                    // 创建属性列表
                    DSetCreatPropList plist;
                    if (spec.name != "baseline_none") {
                        if (is_target) {
                            // chunk 设置
                            std::vector<hsize_t> chunk = dims;
                            if (chunk.size() == 0) chunk = {1};
                            const hsize_t MAX_ELEMS = 1024*1024;
                            hsize_t prod = 1;
                            for (auto d : chunk) prod *= (d>0?d:1);
                            while (prod > MAX_ELEMS) {
                                for (auto &c : chunk) {
                                    if (c > 1) { c = (c+1)/2; }
                                }
                                prod = 1;
                                for (auto d : chunk) prod *= (d>0?d:1);
                            }
                            plist.setChunk((unsigned)chunk.size(), chunk.data());
                            // 应用过滤器
                            if (spec.requires_avail && spec.check_id != 0) {
                                if (!H5Zfilter_avail(spec.check_id)) {
                                    std::cerr << "Filter " << spec.name << " not available; writing dataset uncompressed." << std::endl;
                                } else {
                                    // // 设置 SZIP 选项
                                    if (spec.check_id == H5Z_FILTER_SZIP) {           
                                        herr_t r = H5Pset_szip(plist.getId(), H5_SZIP_NN_OPTION_MASK, 16);
                                        if (r < 0) {
                                            std::cerr << "Warning: failed to set SZIP options for " << child_src_path << "\n";
                                        }
                                    } else {
                                        spec.apply((DSetCreatPropList&)plist);
                                    }
                                }
                            } else {
                                spec.apply((DSetCreatPropList&)plist);
                            }
                        }
                    }

                    // 计算写入时间
                    double write_ms = 0.0;
                    if (is_target && spec.name != "baseline_none") {
                        auto t1 = std::chrono::high_resolution_clock::now();
                        bool okw = create_and_write_dataset(dst, child_src_path, memtid, dims, buf, plist);
                        auto t2 = std::chrono::high_resolution_clock::now();
                        write_ms = std::chrono::duration<double, std::milli>(t2 - t1).count();
                        if (!okw) std::cerr << "Warning: failed to write compressed dataset " << child_src_path << "\n";
                    } else {
                        // 写入非目标数据集或基线（无压缩）
                        auto t1 = std::chrono::high_resolution_clock::now();
                        bool okw = create_and_write_dataset(dst, child_src_path, memtid, dims, buf, plist);
                        auto t2 = std::chrono::high_resolution_clock::now();
                        write_ms = std::chrono::duration<double, std::milli>(t2 - t1).count();
                        if (!okw) std::cerr << "Warning: failed to write dataset " << child_src_path << "\n";                        
                    }
                    compress_ms += write_ms;

                    if (memtid > 0) H5Tclose(memtid);
                }
            }
        };
        // 从根开始递归
        Group root_src = src.openGroup("/");
        Group root_dst = dst.openGroup("/");
        recurse(root_src, root_dst, "/");

        dst.flush(H5F_SCOPE_GLOBAL);
        dst.close();

        // 计算输出文件大小
        uint64_t fsize = 0;
        try {
            fsize = fs::file_size(outpath);
        } catch(...) { fsize = 0; }
       
        // 转换为 MB（MiB）
        double fsize_mb = static_cast<double>(fsize) / (1024.0 * 1024.0);
        return Result{spec.name, fsize_mb, 0.0, compress_ms};
    };

    // 首先生成基线文件
    std::cout << "Generating baseline (no compression) ...\n";
    FilterSpec baseline_spec = specs[0];
    Result baseline_res = run_one(baseline_spec);
    if (baseline_res.file_mb == 0) {
        std::cerr << "Baseline generation failed or file size 0. Aborting.\n";
        return 4;
    }
    std::cout << "Baseline file size: " << baseline_res.file_mb << " bytes\n";

    std::vector<Result> results;
    results.push_back(baseline_res);

    for (size_t i = 1; i < specs.size(); ++i) {
        const auto &spec = specs[i];
        if (spec.requires_avail && spec.check_id != 0) {
            if (!H5Zfilter_avail(spec.check_id)) {
                std::cerr << "Filter " << spec.name << " not available in this HDF5. Skipping.\n";
                continue;
            }
        }
        std::cout << "Running filter: " << spec.name << " ...\n";
        Result r = run_one(spec);
        if (r.file_mb == 0) {
            std::cerr << "Warning: result file size 0 for " << spec.name << "\n";
        }
        // 计算压缩比率
        if (baseline_res.file_mb > 0 && r.file_mb > 0) {
            r.ratio = double(r.file_mb) / double(baseline_res.file_mb);
        } else {
            r.ratio = 0.0;
        }
        results.push_back(r);
        std::cout << " -> size=" << r.file_mb << " MB, ratio=" << r.ratio << ", compress_ms=" << r.compress_ms << "\n";
    }

    // 输出 CSV
    fs::path csv = outdir / "hdf5_filter_results.csv";
    std::ofstream ofs(csv);
    ofs << "filter,file_mb,ratio_compressed_over_baseline,compress_ms\n";
    for (auto &res : results) {
        ofs << res.filter_name << "," << res.file_mb << "," << res.ratio << "," << res.compress_ms << "\n";
    }
    ofs.close();

    std::cout << "Done. Results at: " << csv << "\n";
    return 0;
}


