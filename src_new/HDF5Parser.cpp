#include <iostream>
#include <HDF5Business.h>
#include <FilterRegistry.h>

bool HDF5Parser (H5::Group gsrc, H5::Group gdst, const std::string &gpath, const FilterSpec &spec){
    hsize_t n = gsrc.getNumObjs();
    for (hsize_t i = 0; i < n; ++i) {
        std::string name = gsrc.getObjnameByIdx(i);
        H5G_obj_t type = gsrc.getObjTypeByIdx(i);
        std::string child_src_path = gpath;
        if (child_src_path == "/") child_src_path = "/" + name;
        else child_src_path = gpath + "/" + name;

        if (type == H5G_GROUP) {
            // 创建目标组
            try { gdst.createGroup(name); } 
            catch(...) {std::cerr << "Warning: failed create group " << name << " in " << gpath << "\n"; break;}
            // 复制组属性
            hid_t src_loc = gsrc.getId();
            hid_t dst_loc = gdst.getId();
            copy_attributes(src_loc, name.c_str(), dst_loc);
            // 继续读取子组
            Group ngsrc = gsrc.openGroup(name);
            Group ngdst = gdst.openGroup(name);
            HDF5Parser(ngsrc, ngdst, child_src_path);
        } else if (type == H5G_DATASET) {
            // 解压Raw/Signal数据集：如果是目标数据集则读取原始未被压缩的数据
            bool is_target = true;
            std::vector<char> buf;
            hid_t memtid = -1;
            std::vector<hsize_t> dims;
            DataType cppdtype;

            if (!(name == "Raw" || name == "Signal")) is_target = false;
            bool ok = read_dataset_raw(src, child_src_path, buf, memtid, dims, cppdtype);
            if (!ok) {
                std::cerr << "Warning: failed read dataset " << child_src_path << "\n";
                break;
            }

            // 配置数据集属性列表
            DSetCreatPropList plist;
            if (spec.name != "baseline_none") {
                if (is_target) {   
                    // 配置压缩过滤器属性
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
                    
                    if (spec.requires_avail && spec.check_id != 0) {
                        if (!H5Zfilter_avail(spec.check_id)) {
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
                } //非目标数据集不压缩
            }

            // 写入数据集并统计时间
            
            double write_ms = 0.0;
            auto t1 = std::chrono::high_resolution_clock::now();
            bool okw = create_and_write_dataset(dst, child_src_path, memtid, dims, buf, plist);
            auto t2 = std::chrono::high_resolution_clock::now();
            write_ms = std::chrono::duration<double, std::milli>(t2 - t1).count();
            if (!okw) std::cerr << "Warning: failed to write dataset " << child_src_path << "\n";
            compress_ms += write_ms;
            if (memtid > 0) H5Tclose(memtid);
        } else {
            // 其他对象类型处理 - 此处忽略
        }
    }
}
