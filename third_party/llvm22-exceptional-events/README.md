# Original LLVM 22 exceptional occurrence components

These files are pinned to `llvmorg-22.1.0` from the LLVM project. The eight CodeGen translation units retain upstream lowering and add bounded observation callbacks for original function/ABI variants, expression/call identities, runtime instruction routes, EH carriers, and automatic object cleanup registrations/emissions. EHScopeStack.h adds only a job-local observation accessor for the existing stack depth; its layout remains unchanged. The remaining private headers are upstream dependencies. `original_eh_hooks.hpp` is the cxxlens callback facade.

`CGDecl.cpp` retains the original automatic `VarDecl` when registering a `DestroyObject` cleanup. The copied compiler cleanup carries a per-function registration token; its actual emission retains the normal/EH flag. This proves static declaration correspondence only. Temporaries without that registration remain unbound, and no runtime object version or AST CFG identity follows from a cleanup token.

This is an explicit private static compiler dependency. Objects must precede the matching stock Clang archives and use their assertion, RTTI and exception settings. The provider SDK shared compiler boundary remains stock and cannot claim this instrumentation. Original IR block/instruction ordinals do not identify AST CFG nodes.

The complete upstream license is in LICENSE.TXT. Each modified compiler translation unit carries a modification notice and preserves its upstream license header.

The final original LLVM function supplies complete block and terminator successor
membership independently of call/source binding. Invoke normal and unwind edges
retain separate original occurrence identities. The function-specification
terminate scope is registered immediately after its original push and retired at
its original pop. Each Invoke captures its own selected scope before destination
generation, including cached landing pads; this depth is never exported as an
identity. A direct function-spec termination differs from catch/filter/cleanup
or secondary termination. Graph reachability alone never proves noexcept escape.
