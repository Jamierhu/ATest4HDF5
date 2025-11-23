#include "FilterRegistry.h"

static void initFilters()
{
    //HDF5 内置压缩过滤器和原始数据（无压缩）
    auto& reg = FilterRegistry::instance();
    unsigned int gzip_cp_levs[3] = {1,6,9};

    // baseline
    reg.registerFilter({
        
        "baseline_none",
        [](DSetCreatPropList& p){ /* no compression */ },
        false,
        0
    });

    // gzip
    for(int i = 0; i < 3; ++i){
        reg.registerFilter({
        "shuffle_gzip_lvl" + std::to_string(i+1),
        [](DSetCreatPropList& p){ p.setShuffle(); p.setDeflate(gzip_cp_levs[i]); },
        false,
        H5Z_FILTER_DEFLATE
    });
    }

    // szip
    reg.registerFilter({
        "szip",
        [](DSetCreatPropList& p){},
        false,
        H5Z_FILTER_SZIP 
    });
}


static void registerVBZ()
{
    auto& reg = FilterRegistry::instance();
    //根据代码该版本的压缩级别引用zstd的压缩级别（ZSTD_minCLevel，ZSTD_maxCLevel）->(1,22)，这里测试取1，11，22
    unsigned int vbz_cp_levs[3] = {1,11,22};

    for (int i = 0; i < 3; ++i) {
        reg.registerFilter({
            "vbz_level_" + std::to_string(vbz_cp_levs[i]),
            [i](DSetCreatPropList& p){
                unsigned int cd_vals[4] = {0, vbz_cp_levs[i], 1, 1};
                p.setFilter(
                    FILTER_VBZ_ID,            // VBZ filter ID
                    H5Z_FLAG_MANDATORY,
                    4,
                    cd_vals
                );
            },
            true,      // requires H5Zfilter_avail
            FILTER_VBZ_ID // VBZ filter ID
        });
    }
}

void register_all_filters()
{
    initFilters();
    registerVBZ();
}

