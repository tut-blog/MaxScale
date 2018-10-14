#!/bin/bash

# Check branch name
ref=$(maxscale --version-full 2>&1|grep -o ' - .*'|sed 's/ - //')

if [ -z "$ref" ]
then
    echo "Error: No commit ID in --version-full output"
    exit 1
fi

if [ ! -d MaxScale ]
then
    git clone https://www.github.com/mariadb-corporation/MaxScale.git
    cd MaxScale
    git checkout $ref
    cd ..
fi

cd MaxScale/maxctrl

# Create the scripts that start and stop MaxScale
$maxscale_access_homedir/maxctrl_scripts.sh
chmod +x *.sh

npm i

# Export the value for --basedir where maxscale binaries are located
export MAXSCALE_DIR=/usr
./stop_maxscale.sh

npm test
