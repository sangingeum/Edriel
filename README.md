# Edriel - C++ Networking Project (WIP)


## Prerequisites

```
CMake
Conan
GCC or MSVC with C++20 support
```

## Build Steps

1. Navigate to the project root.
2. Run   
`conan install . -of=build --build=missing` (Release) or    
`conan install . -s build_type=Debug -of=build --build=missing` (Debug)
3. Run    
`cmake --preset conan-release; cmake --build --preset conan-release` (Linux) or   
`cmake --preset conan-default; cmake --build --preset conan-default` (Windows)

## Run

Linux
```bash 
./Edriel
```
Windows
```bash
./Edriel.exe
```
