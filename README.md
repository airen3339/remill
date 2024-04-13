# Remill [![Slack Chat](http://slack.empirehacking.nyc/badge.svg)](https://slack.empirehacking.nyc/)

<p align="center">
     <img src="docs/images/remill_logo.png" />
</p>

Remill is a static binary translator that translates machine code instructions into [LLVM bitcode](http://llvm.org/docs/LangRef.html). It translates AArch64 (64-bit ARMv8), SPARC32 (SPARCv8), SPARC64 (SPARCv9), x86 and amd64 machine code (including AVX and AVX512) into LLVM bitcode. AArch32 (32-bit ARMv8 / ARMv7) support is underway.

Remill focuses on accurately lifting instructions. It is meant to be used as a library for other tools, e.g. [McSema](https://github.com/lifting-bits/mcsema).

## Build Status

[![Build Status](https://img.shields.io/github/workflow/status/lifting-bits/remill/CI/master)](https://github.com/lifting-bits/remill/actions?query=workflow%3ACI)

## Documentation

To understand how Remill works you can take a look at the following resources:

 - [Step-by-step guide on how Remill lifts an instruction](docs/LIFE_OF_AN_INSTRUCTION.md)
 - [How to implement the semantics of an instruction](docs/ADD_AN_INSTRUCTION.md)
 - [The design and architecture of Remill](docs/DESIGN.md)

If you would like to contribute you can check out: [How to contribute](docs/CONTRIBUTING.md)

## Getting Help

If you are experiencing undocumented problems with Remill then ask for help in the `#binary-lifting` channel of the [Empire Hacking Slack](https://slack.empirehacking.nyc/).

## Supported Platforms

Remill is supported on Linux platforms and has been tested on Ubuntu 22.04. Remill also works on macOS, and has experimental support for Windows.

Remill's Linux version can also be built via Docker for quicker testing.

## Dependencies

Most of Remill's dependencies can be provided by the [cxx-common](https://github.com/lifting-bits/cxx-common) repository. Trail of Bits hosts downloadable, pre-built versions of cxx-common, which makes it substantially easier to get up and running with Remill. Nonetheless, the following table represents most of Remill's dependencies.

| Name | Version |
| ---- | ------- |
| [Git](https://git-scm.com/) | Latest |
| [CMake](https://cmake.org/) | 3.14+ |
| [Google Flags](https://github.com/google/glog) | Latest |
| [Google Log](https://github.com/google/glog) | Latest |
| [Google Test](https://github.com/google/googletest) | Latest |
| [LLVM](http://llvm.org/) | 15+ |
| [Clang](http://clang.llvm.org/) | 15 |
| [Intel XED](https://software.intel.com/en-us/articles/xed-x86-encoder-decoder-software-library) | Latest |
| [Python](https://www.python.org/) | 2.7 |
| Unzip | Latest |
| [ccache](https://ccache.dev/) | Latest |

## Getting and Building the Code

### Docker Build

Remill now comes with a Dockerfile for easier testing. This Dockerfile references the [cxx-common](https://github.com/lifting-bits/cxx-common) container to have all pre-requisite libraries available.

The Dockerfile allows for quick builds of multiple supported LLVM, and Ubuntu configurations.

> [!IMPORTANT]
> Not all LLVM and Ubuntu configurations are supported---Please refer to the CI results to get an idea about configurations that are tested and supported. The Docker image should build on both x86_64 and ARM64, but we only test x86_64 in CI. ARM64 _should build_, but if it doesn't, please open an issue.

Quickstart (builds Remill against LLVM 17 on Ubuntu 22.04).

Clone Remill:

```shell
git clone https://github.com/lifting-bits/remill.git
cd remill
```

Build Remill Docker container:

```shell
docker build . -t remill \
     -f Dockerfile \
     --build-arg UBUNTU_VERSION=22.04 \
     --build-arg LLVM_VERSION=17
```

Ensure remill works:

Decode some AMD64 instructions to LLVM:

```shell
docker run --rm -it remill \
     --arch amd64 --ir_out /dev/stdout --bytes c704ba01000000
```

Decode some AArch64 instructions to LLVM:

```shell
docker run --rm -it remill \
     --arch aarch64 --address 0x400544 --ir_out /dev/stdout \
     --bytes FD7BBFA90000009000601891FD030091B7FFFF97E0031F2AFD7BC1A8C0035FD6
```

### On Linux

First, update aptitude and get install the baseline dependencies.

```shell
sudo dpkg --add-architecture i386
sudo apt-get update
sudo apt-get upgrade

sudo apt-get install \
     git \
     python3 \
     wget \
     curl \
     build-essential \
     lsb-release \
     ccache \
     libc6-dev:i386 \
     'libstdc++-*-dev:i386' \
     g++-multilib \
     rpm
```

Next, clone the repository. This will clone the code into the `remill` directory.

```shell
git clone https://github.com/lifting-bits/remill.git
```

Next, we build Remill. This script will create another directory, `remill-build`,
in the current working directory. All remaining dependencies needed
by Remill will be built in the `remill-build` directory.

```shell
./remill/scripts/build.sh
```

Next, we can install Remill. Remill itself is a library, and so there is no real way
to try it. However, you can head on over to the [McSema](https://github.com/lifting-bits/mcsema) repository, which uses Remill for lifting instructions.

```shell
cd ./remill-build
sudo make install
```

We can also build and run Remill's test suite.

```shell
cd ./remill-build
make test_dependencies
make test
```

### Full Source Builds

Sometimes, you want to build everything from source, including the [cxx-common](https://github.com/lifting-bits/cxx-common) libraries remill depends on. To build against a custom cxx-common location, you can use the following `cmake` invocation:

```sh
mkdir build
cd build
cmake  \
  -DCMAKE_INSTALL_PREFIX="<path where remill will install>" \
  -DCMAKE_TOOLCHAIN_FILE="<path to cxx-common directory>/vcpkg/scripts/buildsystems/vcpkg.cmake"  \
  -G Ninja  \
  ..
cmake --build .
cmake --build . --target install
```

The output may produce some CMake warnings about policy CMP0003. These warnings are safe to ignore.

### Common Build Issues

If you see errors similar to the following:

```
fatal error: 'bits/c++config.h' file not found
```

Then you need to install 32-bit libstdc++ headers and libraries. On a Debian/Ubuntu based distribution, You would want to do something like this:

```sh
sudo dpkg --add-architecture i386
sudo apt-get update
sudo apt-get install libc6-dev:i386 libstdc++-10-dev:i386 g++-multilib
```

This error happens because the SPARC32 runtime semantics (the bitcode library which lives in `<install directory>/share/remill/<version>/semantics/sparc32.bc`) are built as 32-bit code, but 32-bit development libraries are not installed by default.

A similar situation occurs when building remill on arm64 Linux. In that case, you want to follow a similar workflow, except the architecture used in `dpkg` and `apt-get` commands  would be `armhf` instead of `i386`.

Another alternative is to disable SPARC32 runtime semantics. To do that, use the `-DREMILL_BUILD_SPARC32_RUNTIME=False` option when invoking `cmake`.
