#!/usr/bin/env python3
from pathlib import Path

path = Path(__file__).with_name("temporary_issue261_patch.py")
text = path.read_text(encoding="utf-8")
old = '''    text = replace_once(
        text,
        "counterexamples:\\n",
        "counterexamples:\\n"
        "  - valid-transfer-rebound-to-different-outer-task\\n",
        "cross-task rebinding counterexample",
    )
'''
new = '''    text = replace_once(
        text,
        "counterexamples: [missing, duplicate, reordered, overlap, gap, extra, post-seal, tamper, limit-overflow, orphan-blob, missing-blob, cross-task, cross-session, stale-ack, foreign-replay, cancel-then-content, capability-absent, downgrade, message-id-collision, ambient-shadow, ng1-resume-alias]\\n",
        "counterexamples: [missing, duplicate, reordered, overlap, gap, extra, post-seal, tamper, limit-overflow, orphan-blob, missing-blob, cross-task, cross-session, stale-ack, foreign-replay, cancel-then-content, capability-absent, downgrade, message-id-collision, ambient-shadow, ng1-resume-alias, valid-transfer-rebound-to-different-outer-task]\\n",
        "cross-task rebinding counterexample",
    )
'''
if text.count(old) != 1:
    raise SystemExit(f"expected one patch-driver counterexample block, found {text.count(old)}")
path.write_text(text.replace(old, new, 1), encoding="utf-8")
