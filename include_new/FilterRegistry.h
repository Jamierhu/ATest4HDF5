#pragma once
#include <string>
#include <functional>
#include <vector>
#include <map>
#include <H5Cpp.h>
#include <vbz-compression/vbz.h>
#include <vbz-compression/vbz_plugin.h>

using namespace H5;

// -----------------------------------------------------------------------------
// FilterSpec: 描述一个 HDF5 压缩过滤器（插件）
// -----------------------------------------------------------------------------
struct FilterSpec {
    std::string name;                                   // 过滤器名字
    std::function<void(DSetCreatPropList&)> apply;      // 如何配置 HDF5 plist
    bool requires_avail = false;                        // 是否需要 H5Zfilter_avail 检查
    unsigned int check_id = 0;                          // HDF5 过滤器 ID (如 32020 for VBZ)
};

// -----------------------------------------------------------------------------
// FilterRegistry 单例：用于注册和管理所有过滤器
// -----------------------------------------------------------------------------
class FilterRegistry {
public:
    // 获取单例
    static FilterRegistry& instance() {
        static FilterRegistry inst;
        return inst;
    }

    // 注册过滤器
    void registerFilter(const FilterSpec& spec) {
        filters.push_back(spec);
    }

    // 获取全部过滤器
    const std::vector<FilterSpec>& getAll() const {
        return filters;
    }

private:
    FilterRegistry() = default;                         // 私有构造
    std::vector<FilterSpec> filters;
};

void register_all_filters();