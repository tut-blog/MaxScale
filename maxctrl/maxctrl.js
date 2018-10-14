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

'use strict';

var maxctrl = require('./lib/core.js')

function print(out) {
    if (out) {
        console.log(out)
    }
}

function err(out) {
    print(out)
    process.exit(1);
}

// Mangle the arguments if we are being called from the command line
if (process.argv[0] == process.execPath) {
    process.argv.shift()
    // The first argument is always the script
    process.argv.shift()
}

maxctrl.execute(process.argv)
    .then(print, err)
