# cxx-module-generator
A tool to easily convert traditional C++ header library to module library.

It converts a header file like
```cpp
// lib.hpp
namespace A {
namespace B {
class C {
  int a, b;
};
}  // namespace B
}  // namespace A
```
into
```cpp
// lib.cppm
module;
#include "lib.hpp"
export module test;
namespace A {
namespace B {
export using A::B::C;
}
}
```
Then you can use `clang++ -march=native -std=c++20 --precompile -c lib.cppm -o lib.pcm` to convert the cppm into pcm.\
At last, you can use the pcm as a C++20 module:
```cpp
import test;

int main() {
  A::B::C c;
  return 0;
}
```
By using modules, the build time decreases dramatically.

## Notice
This tool is still in early development and doesn't always work.
* Symbols with internal linkage can not be exported. However, a workaround is to include them in a header file. This can be enabled with `--internal-linkage-as-header` option.

## Usage
For example, to convert boost/json.hpp, the module name is boost.json, use `cmg boost/json.hpp --name=boost.json --namespace=boost::json --`. Do not forget the `--` at the end of command. It means ignore any `compile_commands.json` in current directory (which most probably you do not have one). 

Most options are the same as any other clang tools.
Use `cmg --help` for more help.

Use `find path/in/subtree -name '*.hpp' | xargs cmg --namespace=a::b --` if you need to run on multiple files.

## Install
**Clang installation is mandatory**
* Download the file from release section
* Put it next to clang executable (for msys2, it should be `msys2/clang64/bin`)

## Tested library
* boost.json
* boost.program_options
* ~~boost.process~~: `std_out` and `std_err` have internal linkage. no matching function for call to `boost::vector_detail::value_at_impl`
