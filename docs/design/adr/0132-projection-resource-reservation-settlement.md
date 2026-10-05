# ADR 0132: Projection resource reservation settlement

Status: accepted

The Monet finite lattice loader invokes several original SDK projections inside one bounded operation. Charging each projector's configured maximum as consumed work keeps a successful earlier reservation unavailable to later projections. The caller needs the successful projector's charged work and a conservative bound on owned and temporary storage, independent of downstream numerical work.

Additive overloads for declarations, body flow, calls, function actions and CFG return a caller-owned `projection_resource_usage`. It is zero on entry and failure, and is published only after successful projection. Operations count the actual checked validation and projection work; retained bytes report the charged conservative geometry, including temporary indexes. These values settle a caller reservation. They are not elapsed time, allocator telemetry, scalar precision, semantic closure or development evidence.

Existing overloads, projection DTO layouts and limit layouts retain their contracts. Original row validation, evidence, source query handles, input flags, conflicts and domain-specific completeness remain unchanged. The public query route borrows immutable row spans while building bounded pointer indexes, then owns the original evidence once. Fixed detached-cell and map geometry is included in finite-row retention before copying; canonical text alone does not bound many short cells.

CFG retains its independent row, evidence and conditioned expansion limits. A further overload accepts the caller's total storage reservation without changing `control_flow_limits`. It preflights borrowed cells and annotation containers, canonical and conditioned-index allowances, source plans, scan handles and missing-scan gaps before ownership. Exceeding a caller reservation returns the existing CFG budget error and leaves usage zero. Cancellation and allocation failure never publish a successful charge.

The independent consumer may release an unused reservation after success and charge returned usage under its existing total budget. Failed projections retain their original error and semantic frontier; a resource counter never converts an unavailable domain to complete. Default caps are preserved.
