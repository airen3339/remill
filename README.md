# Remill


Remill is a static binary translator that translates machine code into [LLVM bitcode](http://llvm.org/docs/LangRef.html). It translates x86 and amd64 machine code (including AVX and AVX512) into LLVM bitcode.

## Build Status

|       | master |
| ----- | ------ |
| Linux | [![Build Status](https://travis-ci.org/trailofbits/remill.svg?branch=master&os=linux)](https://travis-ci.org/trailofbits/remill) |

## Additional Documentation
 
 - [How to contribute](docs/CONTRIBUTING.md)
 - [How to implement the semantics of an instruction](docs/ADD_AN_INSTRUCTION.md)
 - [How instructions are lifted](docs/LIFE_OF_AN_INSTRUCTION.md)
 - [How binaries are represented](docs/CFG_FORMAT.md)
 - [The design and architecture of Remill](docs/DESIGN.md)

## Getting Help

If you are experiencing undocumented problems with Remill then ask for help in the `#tool-remill` channel of the [Empire Hacking Slack](https://empireslacking.herokuapp.com/).

## Supported Platforms

Remill is supported on Linux platforms and has been tested on Ubuntu 16.04 and openSUSE 13.2.

We are actively working on porting Remill to macOS.

## Dependencies

| Name | Version | 
| ---- | ------- |
| [Git](https://git-scm.com/) | Latest |
| [CMake](https://cmake.org/) | 2.8+ |
| [Google Log](https://github.com/google/glog) | 0.3.3 |
| [Google Test](https://github.com/google/googletest) | 1.6.0 |
| [Google Protobuf](https://github.com/google/protobuf) | 2.4.1 |
| [LLVM](http://llvm.org/) | 3.9 |
| [Clang](http://clang.llvm.org/) | 3.9 |
| [Intel XED](https://software.intel.com/en-us/articles/xed-x86-encoder-decoder-software-library) | 2016-02-02 |
| [Python](https://www.python.org/) | 2.7 | 
| [Python Package Index](https://pypi.python.org/pypi) | Latest |
| [python-magic](https://pypi.python.org/pypi/python-magic) | Latest |
| Unzip | Latest |
| [python-protobuf](https://pypi.python.org/pypi/protobuf) | 2.4.1 |
| [Binary Ninja](https://binary.ninja) | Latest |
| [IDA Pro](https://www.hex-rays.com/products/ida) | 6.7+ |

## Getting and Building the Code

### Step 1: Install dependencies

#### On Ubuntu 14.04 and 16.04

##### Setting up LLVM repositories

> Note: Installing LLVM on Ubuntu in such a way that it works for CMake can be tricky. We use LLVM 3.9. What I have found works is to start by removing all versions of all LLVM-related packages. Then, add in the official LLVM repositories (as shown below). Finally, install `llvm-3.9-dev`. If you also need older versions of LLVM-related tools, then re-install them after installing LLVM 3.9.

```shell
UBUNTU_RELEASE=`lsb_release -sc`

wget -O - http://apt.llvm.org/llvm-snapshot.gpg.key|sudo apt-key add -

sudo add-apt-repository "deb http://apt.llvm.org/${UBUNTU_RELEASE}/ llvm-toolchain-${UBUNTU_RELEASE} main"
sudo add-apt-repository "deb http://apt.llvm.org/${UBUNTU_RELEASE}/ llvm-toolchain-${UBUNTU_RELEASE}-3.8 main"
sudo add-apt-repository "deb http://apt.llvm.org/${UBUNTU_RELEASE}/ llvm-toolchain-${UBUNTU_RELEASE}-3.9 main"
```

##### Install packages

```shell
sudo apt-get update
sudo apt-get upgrade

sudo apt-get install git cmake build-essential libgoogle-glog-dev \
     libgtest-dev libprotoc-dev libprotobuf-dev libprotobuf-dev \
     protobuf-compiler python2.7 python-pip llvm-3.9-dev clang-3.9 \
     libc++-dev libc++-dev:i386 libc-dev libc-dev:i386 unzip

sudo pip install --upgrade pip

sudo pip install python-magic 'protobuf==2.4.1'
```

#### On macOS (experimental)

Instructions for building on macOS are not yet available.

### Step 2: Clone the repository

```shell
git clone git@github.com:trailofbits/remill.git
cd remill
```

### Step 3: Install Intel XED

This script will unpack and install Intel XED. It will require `sudo`er permissions. The XED library will be installed into `/usr/local/lib`, and the headers will be installed into `/usr/local/include/intel`.

```shell
sudo ./scripts/install_xed.sh
```

### Step 4: Create auto-generated files

This will compile instruction semantics into bitcode files, and auto-generate protocol buffer files.

```shell
./scripts/bootstrap.sh
```

### Step 5: Build the code

```
mkdir build
cd build
cmake \
    -DCMAKE_C_COMPILER=clang-3.9 \
    -DCMAKE_CXX_COMPILER=clang++-3.9 \
    ..
```

## Try it Out

**TODO(pag):** Make `remill-lift`.
