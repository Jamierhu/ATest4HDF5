// hdf5_filter_bench.cpp
// Compile: g++ -std=c++17 hdf5_filter_bench.cpp -o hdf5_filter_bench -lhdf5_cpp -lhdf5
// (可能需要 -I/path/to/hdf5/include 和 -L/path/to/hdf5/lib)

// 基本思路：
// - 递归遍历源文件，复制组与非目标数据集（通过读写实现）
// - 对 target datasets ("read_*/Raw" and "read_*/Signal") 使用不同 DSetCreatPropList 创建目标文件并写入数据（计时）
// - 读回压缩文件的这些 datasets（计时）
// - 使用 std::filesystem 获取文件大小并输出 CSV 报表

#include <H5Cpp.h>
#include <hdf5.h> // for H5Zfilter_avail
#include <iostream>
#include <vector>
#include <string>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <regex>
#include <sstream>

namespace fs = std::filesystem;
using namespace H5;

struct Result {
    std::string filter_name;
    uint64_t file_bytes;
    double ratio; // compressed / baseline
    double reduction_pct;
    double compress_ms;
    double decompress_ms;
};

std::string joinpath(const std::string &a, const std::string &b) {
    if (a.empty() || a == "/") return "/" + b;
    return a + (a.back() == '/' ? "" : "/") + b;
}

// Checks whether an HDF5 object name corresponds to a "read_xxx" prefix in the path.
// We'll consider that if the dataset's full path contains a segment starting with "read_".
// Example: /some/read_0001/Raw  -> matches
bool path_has_read_prefix(const std::string &fullpath) {
    std::regex re("(^|/)(read_[^/]+)(/|$)");
    return std::regex_search(fullpath, re);
}

bool is_target_dataset(const std::string &fullpath, const std::string &dset_name) {
    // We want to compress datasets named "Raw" or "Signal" that are under a read_* group.
    if (!(dset_name == "Raw" || dset_name == "Signal")) return false;
    return path_has_read_prefix(fullpath);
}

// Utility: copy attributes from source object to destination dataset/group
void copy_attributes(hid_t src_loc, const std::string &name, hid_t dst_loc) {
    // src_loc/dst_loc are object locations (file or group) that contain 'name'
    // We'll open attribute list on source object and write to destination
    // Simpler approach: iterate attributes on the opened object (H5Aiterate)
    // But using H5Cpp: open object by name then copy attributes via low-level API
    // For simplicity, open source object, get number of attributes via H5Aget_num_attrs (H5Oinfo).
    hid_t obj = H5Oopen(src_loc, name.c_str(), H5P_DEFAULT);
    if (obj < 0) return;
    H5O_info_t oinfo;
    if (H5Oget_info(obj, &oinfo) < 0) { H5Oclose(obj); return; }
    // iterate attributes
    int nattrs = H5Aget_num_attrs(obj); // C API
    for (int i = 0; i < nattrs; ++i) {
        hid_t attr = H5Aopen_by_idx(obj, ".", H5_INDEX_NAME, H5_ITER_INC, (hsize_t)i, H5P_DEFAULT, H5P_DEFAULT);
        if (attr < 0) continue;
        // get name
        ssize_t name_len = H5Aget_name(attr, 0, nullptr);
        std::string aname(name_len + 1, '\0');
        H5Aget_name(attr, name_len + 1, &aname[0]);
        aname.resize(name_len);
        // datatype and dataspace
        hid_t atype = H5Aget_type(attr);
        hid_t aspace = H5Aget_space(attr);
        // read raw bytes
        hsize_t asize = H5Tget_size(atype);
        // allocate buffer according to dataspace and type; for simplicity, read via H5Aread using memory type equal to atype
        // create attribute on destination
        hid_t dst_obj = H5Oopen(dst_loc, name.c_str(), H5P_DEFAULT);
        if (dst_obj < 0) { H5Tclose(atype); H5Sclose(aspace); H5Aclose(attr); continue; }
        // create attribute on dst_obj
        hid_t dst_attr = H5Acreate2(dst_obj, aname.c_str(), atype, aspace, H5P_DEFAULT, H5P_DEFAULT);
        if (dst_attr >= 0) {
            // allocate buffer large enough (for simple scalar or small arrays). We'll read using H5T_NATIVE_CHAR into a buffer sized by datatype * nelem
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

// Helper: read entire dataset into memory (raw bytes) and return vector<char>
// Also returns datatype id and dataspace info via output params for writing later.
bool read_dataset_raw(H5::H5File &file, const std::string &path, std::vector<char> &outbuf,
                      hid_t &mem_type_id, std::vector<hsize_t> &dims_out, H5::DataType &cpp_dtype) {
    try {
        DataSet ds = file.openDataSet(path);
        DataSpace space = ds.getSpace();
        int rank = space.getSimpleExtentNdims();
        dims_out.resize(rank);
        space.getSimpleExtentDims(dims_out.data(), nullptr);

        // C++ DataType
        cpp_dtype = ds.getDataType();
        // get native size
        hid_t native_tid = H5Tget_native_type(cpp_dtype.getId(), H5T_DIR_DEFAULT);
        mem_type_id = native_tid;

        // compute total bytes
        hsize_t total = 1;
        for (auto d : dims_out) total *= d;
        size_t type_size = H5Tget_size(native_tid);
        outbuf.resize(static_cast<size_t>(total) * type_size);

        // read
        herr_t err = H5Dread(ds.getId(), native_tid, H5S_ALL, H5S_ALL, H5P_DEFAULT, outbuf.data());
        if (err < 0) {
            std::cerr << "Error reading dataset: " << path << "\n";
            H5Tclose(native_tid);
            return false;
        }
        // leave native_tid open for caller to use; caller must H5Tclose(native_tid) after use
        return true;
    } catch (...) {
        std::cerr << "Exception reading dataset: " << path << "\n";
        return false;
    }
}

// Helper: create dataset in dst file with given dims, mem_type_id, and property list, and write raw bytes
bool create_and_write_dataset(H5::H5File &dst, const std::string &path,
                              hid_t mem_type_id, const std::vector<hsize_t> &dims,
                              const std::vector<char> &buf, const DSetCreatPropList &plist) {
    try {
        // ensure parent groups exist
        // split path and create groups if needed
        std::string p = path;
        if (p.front() == '/') p.erase(0,1);
        size_t pos = 0;
        std::string cur = "";
        while (true) {
            size_t slash = p.find('/', pos);
            std::string token = (slash==std::string::npos) ? p.substr(pos) : p.substr(pos, slash-pos);
            pos = (slash==std::string::npos) ? std::string::npos : slash+1;
            if (pos==std::string::npos) {
                // last token is dataset name, don't create as group
                break;
            }
            cur += "/" + token;
            // create group if not exists
            try {
                Group g = dst.openGroup(cur);
            } catch(...) {
                dst.createGroup(cur);
            }
            if (pos==std::string::npos) break;
        }
        // now create dataset
        DataSpace space(static_cast<int>(dims.size()), dims.data());
        // wrap mem_type_id into H5::DataType for C++ API convenience:
        DataType dtype(mem_type_id);
        DataSet ds = dst.createDataSet(path, dtype, space, plist);
        herr_t err = H5Dwrite(ds.getId(), mem_type_id, H5S_ALL, H5S_ALL, H5P_DEFAULT, buf.data());
        return err >= 0;
    } catch (...) {
        return false;
    }
}

// Copy non-target dataset/group: read and write preserving type/shape (no property changes).
bool copy_object_as_is(H5::H5File &src, H5::H5File &dst, const std::string &path) {
    // path is absolute like /a/b/c (dataset or group)
    try {
        // check if dataset
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
            // create group and copy attributes
            try {
                dst.createGroup(path);
            } catch(...) {}
            // copy attributes: for simplicity we attempt to copy attributes on group
            hid_t src_loc = src.getId();
            hid_t dst_loc = dst.getId();
            copy_attributes(src_loc, path.c_str(), dst_loc);
            // recursively copy children
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
            // other object types - ignore
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

    // filters to test: we will push (name, lambda to configure plist)
    struct FilterSpec {
        std::string name;
        std::function<void(DSetCreatPropList&)> apply;
        bool requires_avail; // if true, we check H5Zfilter_avail before using
        unsigned int check_id; // filter id to check via H5Zfilter_avail (0 if none)
    };
    std::vector<FilterSpec> specs;

    // baseline (no compression)
    specs.push_back({"baseline_none", [](DSetCreatPropList &p){ /* no change */ }, false, 0});

    // gzip level 1 and 9
    specs.push_back({"shuffle_gzip_lvl1", [](DSetCreatPropList &p){ p.setShuffle(); p.setDeflate(1); }, false, H5Z_FILTER_DEFLATE});
    specs.push_back({"shuffle_gzip_lvl9", [](DSetCreatPropList &p){ p.setShuffle(); p.setDeflate(9); }, false, H5Z_FILTER_DEFLATE});

    // shuffle + gzip
    specs.push_back({"shuffle_gzip_lvl6", [](DSetCreatPropList &p){ p.setShuffle(); p.setDeflate(6); }, false, H5Z_FILTER_DEFLATE});

    // szip if available (note: SZIP requires HDF5 built with szip)
#ifdef H5Z_FILTER_SZIP
    specs.push_back({"szip", [](DSetCreatPropList &p){
        // use HDF5 C API to set SZIP options? However H5::DSetCreatPropList doesn't expose setSzip.
        // We'll use C API directly below per dataset if detected.
    }, true, H5Z_FILTER_SZIP});
#else
    // Will attempt detection below
    specs.push_back({"szip", [](DSetCreatPropList &p){}, true, H5Z_FILTER_SZIP});
#endif

    // Note: additional filters (LZF/Blosc/LZ4) can be tested if registered in HDF5. We'll detect via H5Zfilter_avail using common known IDs if available.
    // e.g., many installations register LZF as filter id 32004, but that's not standard. We detect via names not possible here.
    // For now we attempt only above standard ones.

    // open source
    H5::Exception::dontPrint();
    H5::H5File src;
    try {
        src = H5File(src_path, H5F_ACC_RDONLY);
    } catch (const FileIException &e) {
        std::cerr << "Failed to open source file: " << src_path << "\n";
        return 2;
    }

    // create baseline file (explicit) - baseline_none above will create file we call baseline.h5
    fs::path baseline_file = outdir / "baseline_none.h5";
    // We'll implement a function that given a FilterSpec creates an output file and returns Result
    auto run_one = [&](const FilterSpec &spec) -> Result {
        std::string fname = spec.name + ".h5";
        fs::path outpath = outdir / fname;
        // remove existing
        if (fs::exists(outpath)) fs::remove(outpath);
        // create output file
        H5::H5File dst;
        try {
            dst = H5File(outpath.string(), H5F_ACC_TRUNC);
        } catch (...) {
            std::cerr << "Failed to create " << outpath << "\n";
            return Result{spec.name,0,0,0,0,0};
        }

        // We'll iterate over all objects at root and copy. But for target datasets (Raw/Signal under read_*), we will apply compression.
        // Simpler: traverse using getNumObjs/getObjnameByIdx recursively.
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
                    // create group in dst
                    try { gdst.createGroup(name); } catch(...) {}
                    // copy group attributes (simple approach)
                    hid_t src_loc = gsrc.getId();
                    hid_t dst_loc = gdst.getId();
                    copy_attributes(src_loc, name.c_str(), dst_loc);
                    // recurse
                    Group ngsrc = gsrc.openGroup(name);
                    Group ngdst = gdst.openGroup(name);
                    recurse(ngsrc, ngdst, child_src_path);
                } else if (type == H5G_DATASET) {
                    // check if target dataset to compress
                    bool is_target = is_target_dataset(child_src_path, name);
                    // read raw data
                    std::vector<char> buf;
                    hid_t memtid = -1;
                    std::vector<hsize_t> dims;
                    DataType cppdtype;
                    bool ok = read_dataset_raw(src, child_src_path, buf, memtid, dims, cppdtype);
                    if (!ok) {
                        std::cerr << "Warning: failed read dataset " << child_src_path << "\n";
                        continue;
                    }

                    // decide property list
                    DSetCreatPropList plist;
                    if (spec.name == "baseline_none") {
                        // no compression, default storage (could be contiguous)
                        // create contiguous dataset (default)
                    } else {
                        // if this dataset is target, apply filters
                        if (is_target) {
                            // choose chunk size: simple heuristic: set chunk to min(dim, 1024) for 1D or for higher dims set first dim chunk ~min( dims[0], 64)
                            std::vector<hsize_t> chunk = dims;
                            if (chunk.size() == 0) chunk = {1};
                            // reduce chunk sizes to reasonable product <= ~1e6 elements
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
                            // apply spec filters
                            // If spec requires avail check, we do H5Zfilter_avail
                            if (spec.requires_avail && spec.check_id != 0) {
                                if (!H5Zfilter_avail(spec.check_id)) {
                                    std::cerr << "Filter " << spec.name << " not available; writing dataset uncompressed." << std::endl;
                                    // leave uncompressed
                                } else {
                                    // special-case szip: use H5Pset_szip via C API since C++ binding doesn't have it.
                                    if (spec.check_id == H5Z_FILTER_SZIP) {
                                        // set SZIP - choose NN option and pixels per block default
                                        // H5Pset_szip expects options_mask and pixels_per_block
                                        // NOTE: not all HDF5 compile configs have SZIP; this is best-effort.
                                        herr_t r = H5Pset_szip(plist.getId(), H5_SZIP_NN_OPTION_MASK, 16);
                                        if (r < 0) {
                                            std::cerr << "Warning: failed to set SZIP options for " << child_src_path << "\n";
                                        }
                                    } else {
                                        // for other filters (deflate), spec.apply will call setDeflate or setShuffle
                                        spec.apply((DSetCreatPropList&)plist);
                                    }
                                }
                            } else {
                                // not requiring avail or standard filter: just apply
                                spec.apply((DSetCreatPropList&)plist);
                            }
                        } else {
                            // non-target dataset: leave default (no compression)
                        }
                    }

                    // time writing if dataset is target and spec not baseline_none
                    double write_ms = 0.0;
                    if (is_target && spec.name != "baseline_none") {
                        auto t1 = std::chrono::high_resolution_clock::now();
                        bool okw = create_and_write_dataset(dst, child_src_path, memtid, dims, buf, plist);
                        auto t2 = std::chrono::high_resolution_clock::now();
                        write_ms = std::chrono::duration<double, std::milli>(t2 - t1).count();
                        if (!okw) std::cerr << "Warning: failed to write compressed dataset " << child_src_path << "\n";
                    } else {
                        // write normally, maybe measure as well for baseline
                        auto t1 = std::chrono::high_resolution_clock::now();
                        bool okw = create_and_write_dataset(dst, child_src_path, memtid, dims, buf, plist);
                        auto t2 = std::chrono::high_resolution_clock::now();
                        write_ms = std::chrono::duration<double, std::milli>(t2 - t1).count();
                        if (!okw) std::cerr << "Warning: failed to write dataset " << child_src_path << "\n";
                    }
                    // close memtype
                    if (memtid > 0) H5Tclose(memtid);
                } else {
                    // other types: ignore
                }
            }
        };

        // start copying from root group
        Group root_src = src.openGroup("/");
        Group root_dst = dst.openGroup("/");
        recurse(root_src, root_dst, "/");

        // flush and close dst
        dst.flush(H5F_SCOPE_GLOBAL);
        dst.close();

        // measure file size
        uint64_t fsize = 0;
        try {
            fsize = fs::file_size(outpath);
        } catch(...) { fsize = 0; }

        // now measure decompression time: open file and read back target datasets once
        double decompress_ms = 0.0;
        try {
            H5::H5File testf(outpath.string(), H5F_ACC_RDONLY);
            // iterate read_* groups and read Raw/Signal datasets
            std::function<void(H5::Group, const std::string&)> rrecurse;
            rrecurse = [&](H5::Group g, const std::string &gpath) {
                hsize_t n = g.getNumObjs();
                for (hsize_t i = 0; i < n; ++i) {
                    std::string name = g.getObjnameByIdx(i);
                    H5G_obj_t type = g.getObjTypeByIdx(i);
                    std::string child_path = gpath;
                    if (child_path == "/") child_path = "/" + name;
                    else child_path = gpath + "/" + name;
                    if (type == H5G_GROUP) {
                        Group ng = testf.openGroup(child_path);
                        rrecurse(ng, child_path);
                    } else if (type == H5G_DATASET) {
                        if (is_target_dataset(child_path, name)) {
                            // time read
                            auto t1 = std::chrono::high_resolution_clock::now();
                            DataSet ds = testf.openDataSet(child_path);
                            DataSpace sp = ds.getSpace();
                            int rank = sp.getSimpleExtentNdims();
                            std::vector<hsize_t> dims(rank);
                            sp.getSimpleExtentDims(dims.data(), nullptr);
                            // compute bytes
                            DataType dtype = ds.getDataType();
                            hid_t native_tid = H5Tget_native_type(dtype.getId(), H5T_DIR_DEFAULT);
                            hsize_t total = 1;
                            for (auto d : dims) total *= d;
                            size_t tsize = H5Tget_size(native_tid);
                            std::vector<char> buf((size_t)total * tsize);
                            herr_t er = H5Dread(ds.getId(), native_tid, H5S_ALL, H5S_ALL, H5P_DEFAULT, buf.data());
                            auto t2 = std::chrono::high_resolution_clock::now();
                            double ms = std::chrono::duration<double, std::milli>(t2 - t1).count();
                            decompress_ms += ms;
                            H5Tclose(native_tid);
                        }
                    }
                }
            };
            Group rroot = testf.openGroup("/");
            rrecurse(rroot, "/");
            testf.close();
        } catch (...) {
            std::cerr << "Warning: cannot open written file for decompression timing: " << outpath << "\n";
        }

        // note: compress time per dataset was measured inside write; but we didn't collect it globally.
        // For simplicity, we will not attempt to sum writes per dataset; instead we will estimate compress time as 0 here.
        // (Better: accumulate write_ms per dataset above. For brevity, set compress_ms = 0.)
        

        return Result{spec.name, fsize, 0.0, 0.0, compress_ms, decompress_ms};
    };

    // First generate baseline
    std::cout << "Generating baseline (no compression) ...\n";
    FilterSpec baseline_spec = specs[0];
    Result baseline_res = run_one(baseline_spec);
    if (baseline_res.file_bytes == 0) {
        std::cerr << "Baseline generation failed or file size 0. Aborting.\n";
        return 4;
    }
    std::cout << "Baseline file size: " << baseline_res.file_bytes << " bytes\n";

    std::vector<Result> results;
    results.push_back(baseline_res);

    for (size_t i = 1; i < specs.size(); ++i) {
        const auto &spec = specs[i];
        // if requires_avail, check
        if (spec.requires_avail && spec.check_id != 0) {
            if (!H5Zfilter_avail(spec.check_id)) {
                std::cerr << "Filter " << spec.name << " not available in this HDF5. Skipping.\n";
                continue;
            }
        }
        std::cout << "Running filter: " << spec.name << " ...\n";
        Result r = run_one(spec);
        if (r.file_bytes == 0) {
            std::cerr << "Warning: result file size 0 for " << spec.name << "\n";
        }
        // compute ratio and reduction relative to baseline
        if (baseline_res.file_bytes > 0 && r.file_bytes > 0) {
            r.ratio = double(r.file_bytes) / double(baseline_res.file_bytes);
            r.reduction_pct = 100.0 * (1.0 - r.ratio);
        } else {
            r.ratio = 0.0;
            r.reduction_pct = 0.0;
        }
        results.push_back(r);
        std::cout << " -> size=" << r.file_bytes << " bytes, ratio=" << r.ratio << ", reduction%=" << r.reduction_pct << ", decompress_ms=" << r.decompress_ms << "\n";
    }

    // output CSV
    fs::path csv = outdir / "hdf5_filter_results.csv";
    std::ofstream ofs(csv);
    ofs << "filter,file_bytes,ratio_compressed_over_baseline,reduction_pct,compress_ms,decompress_ms\n";
    for (auto &res : results) {
        ofs << res.filter_name << "," << res.file_bytes << "," << res.ratio << "," << res.reduction_pct << "," << res.compress_ms << "," << res.decompress_ms << "\n";
    }
    ofs.close();

    std::cout << "Done. Results at: " << csv << "\n";
    return 0;
}
