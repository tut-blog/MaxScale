/*
 * Copyright (c) 2018 MariaDB Corporation Ab
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
#pragma once

#include <maxscale/ccdefs.hh>

#include <list>
#include <mutex>
#include <string>
#include <unordered_map>

#include <maxscale/service.h>

using namespace maxscale;

/** This contains the database to server mapping */
typedef std::unordered_map<std::string, SERVER*> ServerMap;
typedef std::unordered_map<uint64_t, SERVER*>    BinaryPSMap;
typedef std::unordered_map<uint32_t, uint32_t>   PSHandleMap;

class Shard
{
public:
    Shard();
    ~Shard();

    /**
     * @brief Add a database location
     *
     * @param db     Database to add
     * @param target Target where database is located
     *
     * @return True if location was added
     */
    bool add_location(std::string db, SERVER* target);

    /**
     * @brief Retrieve the location of a database
     *
     * @param db Database to locate
     *
     * @return The database or NULL if no server contains the database
     */
    SERVER* get_location(std::string db);

    void     add_statement(std::string stmt, SERVER* target);
    void     add_statement(uint32_t id, SERVER* target);
    void     add_ps_handle(uint32_t id, uint32_t handle);
    uint32_t get_ps_handle(uint32_t id);
    bool     remove_ps_handle(uint32_t id);
    SERVER*  get_statement(std::string stmt);
    SERVER*  get_statement(uint32_t id);
    bool     remove_statement(std::string stmt);
    bool     remove_statement(uint32_t id);

    /**
     * @brief Change the location of a database
     *
     * @param db     Database to relocate
     * @param target Target where database is relocated to
     */
    void replace_location(std::string db, SERVER* target);

    /**
     * @brief Check if shard contains stale information
     *
     * @param max_interval The maximum lifetime of the shard
     *
     * @return True if the shard is stale
     */
    bool stale(double max_interval) const;

    /**
     * @brief Check if shard is empty
     *
     * @return True if shard contains no locations
     */
    bool empty() const;

    /**
     * @brief Retrieve all database to server mappings
     *
     * @param keys A map where the database to server mappings are added
     */
    void get_content(ServerMap& dest);

    /**
     * @brief Check if this shard is newer than the other shard
     *
     * @param shard The other shard to check
     *
     * @return True if this shard is newer
     */
    bool newer_than(const Shard& shard) const;

private:
    ServerMap   m_map;
    ServerMap   stmt_map;
    BinaryPSMap m_binary_map;
    PSHandleMap m_ps_handles;
    time_t      m_last_updated;
};

typedef std::unordered_map<std::string, Shard> ShardMap;

class ShardManager
{
public:
    ShardManager();
    ~ShardManager();

    /**
     * @brief Retrieve or create a shard
     *
     * @param user         User whose shard to retrieve
     * @param max_lifetime The maximum lifetime of a shard
     *
     * @return The latest version of the shard or a newly created shard if no
     * old version is available
     */
    Shard get_shard(std::string user, double max_lifetime);

    /**
     * @brief Update the shard information
     *
     * The shard information is updated if the new shard contains more up to date
     * information than the one stored in the shard manager.
     *
     * @param shard New version of the shard
     * @param user  The user whose shard this is
     */
    void update_shard(Shard& shard, std::string user);

private:
    mutable std::mutex m_lock;
    ShardMap           m_maps;
};
