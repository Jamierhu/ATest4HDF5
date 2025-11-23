#include <HDF5Business.h>

Result HDF5Business::find_result(const std::string &filter_name) {
    for (const auto &result : results) {
        if (result.filter_name == filter_name) {
            return result;
        }
    }
    return Result();
}



// 复制源文件中指定对象的所有属性到目标文件中的对应对象
void copy_attributes(hid_t src_loc, const std::string &name, hid_t dst_loc) {
    hid_t obj = H5Oopen(src_loc, name.c_str(), H5P_DEFAULT);
    if (obj < 0) return;
    H5O_info_t oinfo;
    if (H5Oget_info(obj, &oinfo) < 0) { H5Oclose(obj); return; }
    // 遍历对象的所有属性
    int nattrs = H5Aget_num_attrs(obj); // C API
    for (int i = 0; i < nattrs; ++i) {
        hid_t attr = H5Aopen_by_idx(obj, ".", H5_INDEX_NAME, H5_ITER_INC, (hsize_t)i, H5P_DEFAULT, H5P_DEFAULT);
        if (attr < 0) continue;
        // 获取属性名称
        ssize_t name_len = H5Aget_name(attr, 0, nullptr);
        std::string aname(name_len + 1, '\0');
        H5Aget_name(attr, name_len + 1, &aname[0]);
        aname.resize(name_len);
        // 获取属性的数据类型和数据空间
        hid_t atype = H5Aget_type(attr);
        hid_t aspace = H5Aget_space(attr);
        // 获取属性的原始字节大小
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

bool read_dataset_raw(H5::H5File &file, const std::string &path, std::vector<char> &outbuf,
                      hid_t &mem_type_id, std::vector<hsize_t> &dims_out, H5::DataType &cpp_dtype) {
    try {
        DataSet ds = file.openDataSet(path);
        DataSpace space = ds.getSpace();
        int rank = space.getSimpleExtentNdims();
        dims_out.resize(rank);
        space.getSimpleExtentDims(dims_out.data(), nullptr);
        cpp_dtype = ds.getDataType();
        hid_t native_tid = H5Tget_native_type(cpp_dtype.getId(), H5T_DIR_DEFAULT);
        mem_type_id = native_tid;

        // 计算总元素数量以分配缓冲区
        hsize_t total = 1;
        for (auto d : dims_out) total *= d;
        size_t type_size = H5Tget_size(native_tid);
        outbuf.resize(static_cast<size_t>(total) * type_size);

        //读取数据集内容到缓冲区
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

// 创建并写入数据集
bool create_and_write_dataset(H5::H5File &dst, const std::string &path,
                              hid_t mem_type_id, const std::vector<hsize_t> &dims,
                              const std::vector<char> &buf, const DSetCreatPropList &plist) {
    try {
        // 验证父组是否存在
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
            // 如果组不存在则创建
            try {
                Group g = dst.openGroup(cur);
            } catch(...) {
                dst.createGroup(cur);
            }
            if (pos==std::string::npos) break;
        }
        // 创建目标数据集并写入数据
        DataSpace space(static_cast<int>(dims.size()), dims.data());
        DataType dtype(mem_type_id);
        DataSet ds = dst.createDataSet(path, dtype, space, plist);
        herr_t err = H5Dwrite(ds.getId(), mem_type_id, H5S_ALL, H5S_ALL, H5P_DEFAULT, buf.data());
        return err >= 0;
    } catch (...) {
        return false;
    }
}

// 复制源文件中指定对象的所有属性到目标文件中的对应对象
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
