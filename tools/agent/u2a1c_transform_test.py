#!/usr/bin/env python3
from u2a1c_transform_common import read, write

rel = "tests/unit/sdk/sqlite_same_process_shm_mapping_registry_test.cpp"
t = read(rel)
if "owned mapped result validates after ordinary family quarantine" not in t:
    old = '''\t\t\tif (mapped)
\t\t\t{
\t\t\t\tauto committed = setup.fixture.registry->commit_reader_map(
\t\t\t\t\t*setup.fixture.family_pin,
\t\t\t\t\t*bound,
\t\t\t\t\tsqlite_same_process_shm_lease_test_peer::reader_attachment_map(
\t\t\t\t\t\trequest,
\t\t\t\t\t\tsetup.holder.generation(),
\t\t\t\t\t\tmapping(setup.writer_attempt.native_page.get()),
\t\t\t\t\t\teffect->identity()),
\t\t\t\t\tsetup.session);
\t\t\t\trequire(committed && committed->formed_group() && !bound->valid(),
\t\t\t\t\t"owned mapped terminal survives ordinary family quarantine");
\t\t\t}'''
    new = '''\t\t\tif (mapped)
\t\t\t{
\t\t\t\tauto receipt = sqlite_same_process_shm_reader_receipt_validator::validate(
\t\t\t\t\t*setup.fixture.registry,
\t\t\t\t\t*setup.fixture.family_pin,
\t\t\t\t\t*bound,
\t\t\t\t\towner.scope,
\t\t\t\t\towner.callback_identity,
\t\t\t\t\t*effect,
\t\t\t\t\t0,
\t\t\t\t\tsetup.writer_attempt.native_page.get(),
\t\t\t\t\t0,
\t\t\t\t\tidentity("test.registry.qualified-mapped-shm-object", 78U),
\t\t\t\t\tidentity("test.registry.qualified-mapped-shm-entry", 78U),
\t\t\t\t\tidentity("test.registry.qualified-mapped-device", 78U),
\t\t\t\t\tidentity("test.registry.qualified-mapped-mount", 78U));
\t\t\t\trequire(receipt && effect->valid(),
\t\t\t\t\t"owned mapped result validates after ordinary family quarantine");
\t\t\t\tauto committed = setup.fixture.registry->commit_reader_map(
\t\t\t\t\t*setup.fixture.family_pin, *bound, *receipt, setup.session);
\t\t\t\trequire(committed && committed->formed_group() && !bound->valid(),
\t\t\t\t\t"owned mapped terminal survives ordinary family quarantine");
\t\t\t}'''
    if old not in t:
        raise RuntimeError("qualified mapped success branch not found")
    t = t.replace(old, new, 1)
write(rel, t)
