# LLVM 22 original template event components

These source components originate from llvm-project tag `llvmorg-22.1.0`.
They require that compatible LLVM/Clang public interface and static archives.
The four `.cpp` files add original compiler event callbacks at candidate
selection and legacy constant interpreter roots/invocations. Private headers
are unchanged upstream dependencies. The upstream license is LICENSE.TXT.

Compile the components with the installed LLVM assertion, RTTI and exception
ABI settings. Objects are linked before the original static component archives.
Stock clang-cpp and bytecode interpreter routes do not claim event closure.
