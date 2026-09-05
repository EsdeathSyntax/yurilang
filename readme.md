# YuriLang
![YuriLang](https://github.com/EsdeathSyntax/yurilang/blob/main/art/yuri.png "Made with love")

YuriLang is a custom LLVM-integrated programming & scripting language written from scratch in C++ designed for raw performance and efficiency.

## Core Architecture & Features

- **LLVM JIT Engine**: Direct intermediate representation (IR) generation and fast native machine code execution.
- **Robust Type Checking**: Supports core primitives (`i32`, `i64`, `f32`, `f64`, `bool`, `string`), custom data structures, and native nullable type modifiers (`?`).
- **Clean Modular Design**: Includes a modular framework for .yuri files, all out of the box.

## Quick Start
Requirements:
 - Linux environment with POSIX-compliant socket support
 - C++23 compliant compiler
 - LLVM development packages
 - CMake 3.20 or higher

## Building
Clone & Compile using CMake for example:

```
git clone https://github.com/EsdeathSyntax/yurilang.git
cd YuriLang
python3 opts.py build
```


## Compiling a file/directory:
```
./build/yurilang examples
```
or
```
./build/yurilang main.yuri
```

Distributed under the MIT License. See the LICENSE file for details.