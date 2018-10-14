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

#include "tempfile.hh"
#include <string.h>
#include <unistd.h>
#include <maxbase/assert.h>

namespace
{

static char NAME_TEMPLATE[] = "/tmp/XXXXXX";
}

TempFile::TempFile()
    : m_fd(-1)
    , m_name(NAME_TEMPLATE)
{
    m_fd = mkstemp((char*)m_name.c_str());
    mxb_assert(m_fd != -1);
}

TempFile::~TempFile()
{
    int rc = unlink(m_name.c_str());
    mxb_assert(rc != -1);
    close(m_fd);
}

void TempFile::write(const void* pData, size_t count)
{
    int rc = ::write(m_fd, pData, count);
    mxb_assert(rc != -1);
    mxb_assert((size_t)rc == count);
}

void TempFile::write(const char* zData)
{
    write(zData, strlen(zData));
}
