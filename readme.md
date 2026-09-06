# YuriLang
![YuriLang](https://github.com/EsdeathSyntax/yurilang/blob/main/art/yuri.png "Made with love")

YuriLang is a custom LLVM-integrated programming & scripting language written from scratch in C++ designed for raw performance and efficiency.

## Core Architecture & Features

- **LLVM JIT Engine**: Direct intermediate representation (IR) generation and fast native machine code execution.
- **Robust Type Checking**: Supports core primitives (`int`, `float`, `str`, `bool`, `array`), custom data structures, and nullable type modifiers (`?`).

## Quick Start
Requirements:
 - Linux environment (as of 9/6/26)
 - C++23 compliant compiler
 - LLVM development packages
 - CMake 3.20 or higher

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

Distributed under the MIT License. See the LICENSE file for details.

## Notes/Disclaimers/Links
 - [Full documentation](https://esdeath.org/yurilang/documentation)
 - Generative AI was involved in this project.