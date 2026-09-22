# cc-backend
Multi-target compiler backend: IR -> / x86_64 / ARM64 / ARM.
## Build
```sh
make
```
## Usage
```sh
./cc-backend <target> <input.ir> <output.o>
```
Targets: `x86`, `x86_64`, `arm`, `arm64`, `riscv`.
