#!/bin/bash

# do the real building work
# this script is executed on build VM

set -x

cd ./MaxScale

mkdir _build
cd _build
cmake ..  $cmake_flags
make || exit 1

if [[ "$cmake_flags" =~ "BUILD_TESTS=Y" ]]
then
    # We don't care about memory leaks in the tests (e.g. servers are never freed)
    export ASAN_OPTIONS=detect_leaks=0
    # All tests must pass otherwise the build is considered a failure
    ctest --output-on-failure || exit 1

    # See if docker is installed and run REST API and MaxCtrl tests if it is
    command -v docker
    if [ $? -eq 0 ]
    then
        export SKIP_SHUTDOWN=Y
        make test_rest_api && make test_maxctrl
        rc=$?
        #docker ps -aq|xargs docker rm -vf

        if [ $rc -ne 0 ]
        then
            cat maxscale_test/*.output maxscale_test/log/maxscale/*.log
            exit 1
        fi
    fi
fi

# Never strip binaries
sudo rm -rf /usr/bin/strip
sudo touch /usr/bin/strip
sudo chmod a+x /usr/bin/strip

sudo make package
res=$?
if [ $res != 0 ] ; then
	echo "Make package failed"
	exit $res
fi

sudo rm ../CMakeCache.txt
sudo rm CMakeCache.txt

echo "Building tarball..."
cmake .. $cmake_flags -DTARBALL=Y
sudo make package

cd ..
cp _build/*.rpm .
cp _build/*.gz .

if [ "$build_experimental" == "yes" ]
then
    for component in experimental devel cdc-connector
    do
        cd _build
        rm CMakeCache.txt
        cmake ..  $cmake_flags -DTARGET_COMPONENT=$component
        sudo make package
        cd ..
        cp _build/*.rpm .
	    cp _build/*.gz .
    done
fi

if [ "$BUILD_RABBITMQ" == "yes" ] ; then
  cmake ../rabbitmq_consumer/  $cmake_flags
  sudo make package
  res=$?
  if [ $res != 0 ] ; then
        exit $res
  fi
  cd ..
  cp _build/*.rpm .
  cp _build/*.gz .
fi

sudo rpm -i maxscale*.rpm
