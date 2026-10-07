# ADR 0134: Original entered-file source features

Status: accepted

Freestanding and portability consumers cannot infer a whole source-file domain
from written function bodies or operation populations. Global and field
initializers, defaults, template syntax and unevaluated expressions are eligible
original static observations too.

Retain one bounded original Decl/Stmt/Attr/TypeLoc traversal stream with independent
translation-unit and actual entered-file count/member/state/profile inventories.
Traversal occurrence identity and repeated original-node identity remain separate.
Actual LangOptions::Freestanding is an independent optional atomic unit facet.
Original source, declaration, context, entity, selected call and type bindings use
the existing collector's identity maps and retain missing targets independently.

Complete file closure requires exact original entered-file membership, its frozen
file/snapshot association, complete eligible traversal and no unresolved valid
source attribution. Compiler-invalid source ranges may be excluded from file
attribution only through a supported exact implicit declaration/attribute, default
activation wrapper or implicit declaration-owned TypeSourceInfo witness. Unknown
origin remains retained at translation-unit scope and does not prove file zero.

The public standalone projection validates every row, preserves conditioned
evidence and conflicts, and measures bounded work and conservative storage peak.
No compiler pointer escapes and no template or expression is evaluated merely to
complete the population. Raw kind names do not classify extensions, facilities,
runtime effects, resource activation or alias/object versions. Missing original
dependencies remain unknown. Released public DTOs and immutable installed prefixes
are unchanged.
