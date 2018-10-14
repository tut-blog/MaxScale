/*
 * Copyright (c) 2016 MariaDB Corporation Ab
 *
 * Use of this software is governed by the Business Source License included
 * in the LICENSE.TXT file and at www.mariadb.com/bsl11.
 *
 * Change Date: 2022-01-01
 *
 * On the date above, in accordance with the Business Source License, use
 * of this software will be governed by version 2 or later of the General
 * Public License.
 */

#define MXS_MODULE_NAME "cache"
#include "lrustoragemt.hh"

LRUStorageMT::LRUStorageMT(const CACHE_STORAGE_CONFIG& config, Storage* pStorage)
    : LRUStorage(config, pStorage)
{
    MXS_NOTICE("Created multi threaded LRU storage.");
}

LRUStorageMT::~LRUStorageMT()
{
}

LRUStorageMT* LRUStorageMT::create(const CACHE_STORAGE_CONFIG& config, Storage* pStorage)
{
    LRUStorageMT* plru_storage = NULL;

    MXS_EXCEPTION_GUARD(plru_storage = new LRUStorageMT(config, pStorage));

    return plru_storage;
}

cache_result_t LRUStorageMT::get_info(uint32_t what,
                                      json_t** ppInfo) const
{
    std::lock_guard<std::mutex> guard(m_lock);

    return LRUStorage::do_get_info(what, ppInfo);
}

cache_result_t LRUStorageMT::get_value(const CACHE_KEY& key,
                                       uint32_t flags,
                                       uint32_t soft_ttl,
                                       uint32_t hard_ttl,
                                       GWBUF**  ppValue) const
{
    std::lock_guard<std::mutex> guard(m_lock);

    return do_get_value(key, flags, soft_ttl, hard_ttl, ppValue);
}

cache_result_t LRUStorageMT::put_value(const CACHE_KEY& key, const GWBUF* pValue)
{
    std::lock_guard<std::mutex> guard(m_lock);

    return do_put_value(key, pValue);
}

cache_result_t LRUStorageMT::del_value(const CACHE_KEY& key)
{
    std::lock_guard<std::mutex> guard(m_lock);

    return do_del_value(key);
}

cache_result_t LRUStorageMT::get_head(CACHE_KEY* pKey, GWBUF** ppHead) const
{
    std::lock_guard<std::mutex> guard(m_lock);

    return LRUStorage::do_get_head(pKey, ppHead);
}

cache_result_t LRUStorageMT::get_tail(CACHE_KEY* pKey, GWBUF** ppTail) const
{
    std::lock_guard<std::mutex> guard(m_lock);

    return LRUStorage::do_get_tail(pKey, ppTail);
}

cache_result_t LRUStorageMT::get_size(uint64_t* pSize) const
{
    std::lock_guard<std::mutex> guard(m_lock);

    return LRUStorage::do_get_size(pSize);
}

cache_result_t LRUStorageMT::get_items(uint64_t* pItems) const
{
    std::lock_guard<std::mutex> guard(m_lock);

    return LRUStorage::do_get_items(pItems);
}
