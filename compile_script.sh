#!/bin/bash -x 

################## Prerequisite ################## 

# Python 2.7
# Openmpi
# libtool
# pkg-config         
# CMake
# Flex    
# Bison

################## Actual scripts starts from here ##################

# Where to compile
CUR_DIR="$PWD"
mkdir ${CUR_DIR}/install
INSTALL_DIR=${CUR_DIR}/install

##### Downloading everything #####

git clone https://github.com/codes-org/codes --branch=kronos-union
git clone https://github.com/ross-org/ross --depth=20 --branch=at_gvt_arbitrary_function
git clone https://github.com/pmodels/argobots --depth=1
git clone https://github.com/SPEAR-UIC/Union --branch=mfnetsim

curl -L https://sourceforge.net/projects/conceptual/files/conceptual/1.5.1b/conceptual-1.5.1b.tar.gz -o conceptual-1.5.1b.tar.gz
tar xvf conceptual-1.5.1b.tar.gz

curl -L https://archives.boost.io/release/1.87.0/source/boost_1_87_0.tar.gz -o boost_1_87_0.tar.gz
tar xvf boost_1_87_0.tar.gz

##### COMPILING #####

# Compile ROSS
mkdir ross/build
pushd ross/build
cmake .. -DROSS_BUILD_MODELS=ON -DCMAKE_INSTALL_PREFIX="${INSTALL_DIR}/ross" \
  -DCMAKE_C_COMPILER=mpicc -DCMAKE_BUILD_TYPE=Debug -DCMAKE_C_FLAGS="-g -Wall"
#make VERBOSE=1
make install -j4
err=$?
[[ $err -ne 0 ]] && exit $err
popd

# Compile Boost
pushd boost_1_87_0
./bootstrap.sh --prefix="${INSTALL_DIR}/boost" --with-libraries=python
./b2 install
popd

# Compile SWM
pushd swm-workloads/swm
./prepare.sh
mkdir build
pushd build
../configure --disable-shared --with-boost="${INSTALL_DIR}/boost" --prefix="${INSTALL_DIR}/swm" CC=mpicc CXX=mpicxx CFLAGS=-g CXXFLAGS=-g
#make V=1 && make install
make -j4 && make install
err=$?
[[ $err -ne 0 ]] && exit $err
popd && popd

# Compile Argobots
pushd argobots
./autogen.sh
mkdir build
pushd build
../configure --disable-shared --prefix="${INSTALL_DIR}/argobots" CC=mpicc CXX=mpicxx CFLAGS=-g CXXFLAGS=-g
#make V=1 && make install
make -j4 && make install
err=$?
[[ $err -ne 0 ]] && exit $err
popd && popd

# Compile Conceptual
pushd conceptual-1.5.1b
PYTHON=python2 ./configure --prefix="${INSTALL_DIR}/conceptual" 
if make && make install; then
    echo "Make and install succeeded."
else
    echo "Make or install failed. Trying 'make all-am && make install-am'..."
    make all-am && make install-am
fi
err=$?
[[ $err -ne 0 ]] && exit $err
popd


# Compile Union
pushd Union
./prepare.sh
mkdir build
pushd build
../configure --disable-shared --with-conceptual="${INSTALL_DIR}/conceptual" --prefix="${INSTALL_DIR}/union" CC=mpicc CXX=mpicxx
make -j4 && make install
err=$?
[[ $err -ne 0 ]] && exit $err
popd && popd


# Compile CODES
pushd codes
./prepare.sh
mkdir build
pushd build
../configure --with-online=true --with-boost=${INSTALL_DIR}/boost PKG_CONFIG_PATH=${INSTALL_DIR}/argobots/lib/pkgconfig:${INSTALL_DIR}/ross/lib/pkgconfig:${INSTALL_DIR}/union/lib/pkgconfig:${INSTALL_DIR}/swm/lib/pkgconfig --with-union=true --prefix=${INSTALL_DIR}/codes CC=mpicc CXX=mpicxx CFLAGS="-g -O0" CXXFLAGS="-g -O0 -std=c++11" LDFLAGS="-L/usr/lib64 -lstdc++"
make -j4 && make install
err=$?
[[ $err -ne 0 ]] && exit $err
popd && popd


#Compile MFNetSim
pushd mfnetsim
./prepare
mkdir build
pushd build
../configure PKG_CONFIG_PATH=${INSTALL_DIR}/codes/lib/pkgconfig --prefix=${INSTALL_DIR}/mfnetsim CC=mpicc CXX=mpicxx CFLAGS="-g -O0" CXXFLAGS="-g -O0 -std=c++11" LDFLAGS="-L/usr/lib64 -lstdc++"
make -j4 && make install
err=$?
[[ $err -ne 0 ]] && exit $err
popd && popd
