# LLVM 22 original template event components

These source components originate from llvm-project tag `llvmorg-22.1.0`.
They require that compatible LLVM/Clang public interface and static archives.
The five `.cpp` files add original compiler event callbacks at candidate
selection, original sequencing checks and legacy interpreter roots/invocations. Private headers
are unchanged upstream dependencies. The upstream license is LICENSE.TXT.

Compile the components with the installed LLVM assertion, RTTI and exception
ABI settings. Objects are linked before the original static component archives.
Stock clang-cpp and bytecode interpreter routes do not claim event closure.

SemaChecking.cpp additionally retains the actual final unsequenced candidate pair
and exact original argument contexts. ExprConstant.cpp retains pointwise original
object-state observations, with the existing template evaluation-root ordinal
forwarded to an independent bounded parser-phase object observer. Both observers
freeze before extraction and any helper evaluation. Candidate stream closure is
not absence of runtime object violations; APValue absence alone is not an ended
lifetime. Compiler assertion, RTTI and exception flags must match the shipped
release ABI for all five exact translation units.

The actual linked SemaChecking and ExprConstant translation units expose their
functional object-observation route masks independently of template event routes.
A build macro alone does not close an absent object or sequencing stream.
