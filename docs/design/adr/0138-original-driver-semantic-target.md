# ADR 0138: Original driver semantic target on the supported host

Status: accepted.

Contract ID: `clang22-original-driver-semantic-target/1`. Authority is ADR 0112,
the existing compiler/input contracts and the original exact Clang 22 driver and
ASTContext. Write scope is local application preparation and its original native
invocation. Existing build-context and ABI/type descriptors retain their identities
and semantics; there is no new target-layout formula or inferred target model.

Monet's Representation and portability consumers need paired original data-model
observations, including LP64 and LLP64, from the same no-header source. The supported
Linux x86_64 host executes a POSIX Clang driver and the existing native parser.
The driver's semantic target is an input to that parser, independently of the
host's runtime/provider compatibility tuple. Accepting a Windows semantic target
does not add Windows-native execution, MSVC/clang-cl capture or a supported Windows
host/package tuple. The existing support matrix and host bridge remain unchanged.

Preparation retains the exact original compiler version, sealed executable image,
resource directory and builtin headers, selected flags, macro observations, include
search, dependency/source closure, ambiguity checks and existing resource bounds.
The original selected driver's `-dumpmachine` result must be nonempty and is made
explicit in the subsequent native invocation. Architecture-affecting flags remain
present. This binds an implicit driver default as well as an explicit target; it
does not replace failed probes or compiler diagnostics with a host fallback.
An unsupported driver/frontend target fails through the existing typed preparation
or parser terminal. No target is admitted by guessed predefined-macro geometry.

The original native ASTContext supplies actual canonical integer object storage
and actual TargetInfo char, long, pointer, wchar and byte-order observations under
their existing independent profiles. A driver target string or a host sizeof
result supplies none of those values. Target-sensitive ABI, exceptional emitter,
runtime, object and model domains retain their existing complete/partial/unknown
or unsupported states. In particular, accepting an AST target does not close an
unsupported exceptional-emitter route, alias domain or runtime outcome population.
No bound, public DTO layout or original query side channel changes.

The independent consumer may derive proved, disproved, unknown, partial or
conflicting results only after binding those original target/type facets and its
other required axes. Coverage, closure, unresolved reasons, conflicts, guarantees
and provenance remain in ordinary queries and archives. Missing or contradictory
facts never become host-layout defaults or complete empty populations.

Completion order is this admission contract, explicit original target binding,
genuine paired Linux/Windows x64 queries and exact typed storage/data-model checks,
an actual unsupported-driver-target negative, the ordinary application regression,
then main CI. The probes run on the supported Linux host with POSIX paths and no
headers; they preserve compiler and parser diagnostics. Existing output/resource
failure controls remain active. Ordinary Git, build logs and tests are sufficient.
