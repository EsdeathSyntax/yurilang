# YuriLang
![YuriLang](https://github.com/EsdeathSyntax/yurilang/blob/main/art/yuri.png "Made with love")

YuriLang is a custom LLVM-backend programming & scripting language written from scratch in C++ designed for raw performance and efficiency.

## Features

- **Dynamically Typed**: Supports standard primitives (`int`, `float`, `str`, `bool`, `array`), custom data structures, and nullable type modifiers (`?`); you can also go with no types.

## Quick Start
Requirements:
 - Linux environment (as of 9/6/26)
 - C++23 compiler
 - LLVM development packages
 - CMake, preferably latest version

## Building
Clone & Compile using CMake for example (intended method):

```
git clone https://github.com/EsdeathSyntax/yurilang.git
cd YuriLang
python3 opts.py build
```

Please implement/modify the build function in opts.py for other builders such as clang.


## Compiling a file/directory:
```
./build/yurilang examples
```
or
```
./build/yurilang main.yuri
```

Distributed under the MIT License.

## Notes/Disclaimers/Links
 - Full documentation to be added
 - Generative AI was involved in this project.