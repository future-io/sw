# Software Network (SW) #

[![Build status](https://ci.appveyor.com/api/projects/status/3mf8eall4lf764sk/branch/master?svg=true)](https://ci.appveyor.com/project/egorpugin/sw/branch/master)

## Build

1. Download CPPAN client from https://cppan.org/client/
2. Run 
```
git clone https://github.com/SoftwareNetwork/sw
cd sw
cppan --generate .
```
3. Open generated solution file for Visual Studio.

For other build types or OSs, run cppan with config option `cppan --generate . --config gcc8_debug`.

Check out config options at https://github.com/SoftwareNetwork/sw/blob/master/cppan.yml#L7
