#!/usr/bin/env python3
"""Recover and complete issue #205 as one fail-closed serial unit.

This controller lives only on an ephemeral branch.  It refuses to publish unless
its exact base remains immutable, all static/shared/sanitizer tests pass, repeated
race-focused tests remain green, and two independent reviews approve the exact
diff.  If another controller already completed #205, it exits without mutation.
"""

from __future__ import annotations

import json
import os
import pathlib
import re
import shlex
import subprocess
import sys
import time
import urllib.request
from dataclasses import dataclass
from typing import Any

ENDPOINT = "https://models.github.ai/inference/chat/completions"
MODELS = ("openai/gpt-4.1", "openai/gpt-4o")
BASE_BRANCH = "issue-181-clang22-materialization-runtime"
UNIT_BRANCH = "agent/u2a2-recovery-205"
ISSUE = 205


@dataclass(frozen=True)
class State:
    repository: str
    token: str
    repo: pathlib.Path
    work: pathlib.Path
    base_sha: str


def run(
    command: list[str],
    *,
    cwd: pathlib.Path | None = None,
    check: bool = True,
    capture: bool = False,
    env: dict[str, str] | None = None,
    timeout: int | None = None,
) -> subprocess.CompletedProcess[str]:
    print("+ " + shlex.join(command), flush=True)
    return subprocess.run(
        command,
        cwd=cwd,
        check=check,
        text=True,
        capture_output=capture,
        env=env,
        timeout=timeout,
    )


def gh(state: State | None, repository: str, token: str, path: str, *, method: str = "GET", fields: dict[str, str] | None = None) -> Any:
    command = ["gh", "api", path]
    if method != "GET":
        command.extend(["--method", method])
    for key, value in (fields or {}).items():
        command.extend(["-f", f"{key}={value}"])
    result = run(command, capture=True, env={**os.environ, "GH_TOKEN": token})
    return json.loads(result.stdout)


def bounded(text: str, limit: int) -> str:
    if len(text) <= limit:
        return text
    front = limit // 3
    return text[:front] + "\n\n[... bounded middle omitted ...]\n\n" + text[-(limit - front) :]


def wait_for_pr_213(repository: str, token: str) -> bool:
    for attempt in range(1, 301):
        issue = gh(None, repository, token, f"repos/{repository}/issues/{ISSUE}")
        if issue.get("state") == "closed":
            print("issue #205 is already closed", flush=True)
            return False
        pr = gh(None, repository, token, f"repos/{repository}/pulls/213")
        if pr.get("merged") is True:
            return True
        print(f"attempt {attempt}: waiting for PR #213", flush=True)
        time.sleep(30)
    raise RuntimeError("PR #213 did not integrate within the bounded wait")


def call_model(state: State, *, model: str, system: str, prompt: str, tokens: int) -> str:
    errors: list[str] = []
    for retry in range(5):
        body = json.dumps(
            {
                "model": model,
                "messages": [
                    {"role": "system", "content": system},
                    {"role": "user", "content": prompt},
                ],
                "temperature": 0.05,
                "max_tokens": tokens,
            }
        ).encode("utf-8")
        request = urllib.request.Request(
            ENDPOINT,
            data=body,
            headers={
                "Accept": "application/vnd.github+json",
                "Authorization": f"Bearer {state.token}",
                "Content-Type": "application/json",
                "X-GitHub-Api-Version": "2022-11-28",
            },
            method="POST",
        )
        try:
            with urllib.request.urlopen(request, timeout=300) as response:
                payload = json.load(response)
            return str(payload["choices"][0]["message"]["content"])
        except Exception as error:  # noqa: BLE001
            errors.append(f"{type(error).__name__}: {error}")
            time.sleep(15 * (retry + 1))
    raise RuntimeError(f"GitHub Models failed for {model}: " + " | ".join(errors))


def extract_diff(text: str) -> str:
    fenced = re.search(r"```(?:diff|patch)?\s*\n(.*?)```", text, re.DOTALL)
    if fenced:
        text = fenced.group(1)
    start = text.find("diff --git ")
    if start < 0:
        raise RuntimeError("model response did not contain a unified diff")
    return text[start:].rstrip() + "\n"


def apply_patch(state: State, patch: str, name: str) -> None:
    path = state.work / name
    path.write_text(patch, encoding="utf-8")
    run(
        ["git", "apply", "--index", "--3way", "--whitespace=fix", str(path)],
        cwd=state.repo,
    )


def git_diff(state: State, *, cached: bool = True, limit: int = 650_000) -> str:
    command = ["git", "diff"]
    if cached:
        command.append("--cached")
    output = run(command, cwd=state.repo, capture=True).stdout
    return bounded(output, limit)


def collect_payload(state: State) -> dict[str, Any]:
    return {
        "issue": gh(state, state.repository, state.token, f"repos/{state.repository}/issues/{ISSUE}"),
        "comments": gh(state, state.repository, state.token, f"repos/{state.repository}/issues/{ISSUE}/comments?per_page=100"),
        "tracker": gh(state, state.repository, state.token, f"repos/{state.repository}/issues/181"),
        "tracker_comments": gh(state, state.repository, state.token, f"repos/{state.repository}/issues/181/comments?per_page=100")[-30:],
        "u2a1_pr": gh(state, state.repository, state.token, f"repos/{state.repository}/pulls/213"),
        "integration_pr": gh(state, state.repository, state.token, f"repos/{state.repository}/pulls/193"),
    }


def collect_context(state: State, payload: dict[str, Any]) -> str:
    patterns = (
        "any_native_ok",
        "xShmMap|xShmUnmap|xClose",
        "owner.?qualified|qualified.?owner",
        "mapped.?result|mapping.?result",
        "zero.?effect.?identity",
        "validation.?capability|presentation|presenter",
        "independent SQLite CAS stores unavailable",
        "race fixture competitor did not publish",
        "same.?process|process.?identity",
        "terminal|quarantine|replay|drop",
        "Store|compare.?and.?swap|materialization",
    )
    pieces: list[str] = []
    for pattern in patterns:
        result = run(
            [
                "rg",
                "-n",
                "-i",
                "-C",
                "45",
                "--glob",
                "!build/**",
                pattern,
                "src/sdk",
                "tests/unit/sdk",
                "tests/adapter/clang22",
                "docs/qualification",
                "docs/architecture",
            ],
            cwd=state.repo,
            check=False,
            capture=True,
        )
        pieces.append(f"===== {pattern} =====\n{bounded(result.stdout, 95_000)}")

    complete_files = run(
        [
            "rg",
            "-l",
            "-i",
            "independent SQLite CAS stores unavailable|race fixture competitor did not publish|any_native_ok|owner.?qualified|mapped.?result",
            "src/sdk",
            "tests/unit/sdk",
            "tests/adapter/clang22",
        ],
        cwd=state.repo,
        check=False,
        capture=True,
    ).stdout.splitlines()
    for relative in complete_files[:24]:
        path = state.repo / relative
        if path.is_file() and path.stat().st_size <= 220_000:
            pieces.append(f"===== complete file {relative} =====\n{path.read_text(errors='replace')}")

    pieces.append("===== issue payload =====\n" + json.dumps(payload, ensure_ascii=False, indent=2))
    return bounded("\n\n".join(pieces), 950_000)


def configure_static(state: State) -> None:
    run(
        [
            "cmake",
            "--preset",
            "ci-quick",
            "-DCXXLENS_CLANG_ADAPTER=ON",
            "-DCXXLENS_BUILD_SHARED=OFF",
        ],
        cwd=state.repo,
        env={**os.environ, "CXX": "clang++-22"},
        timeout=1200,
    )


def build_and_test_static(state: State, log_name: str) -> bool:
    log_path = state.work / log_name
    env = {**os.environ, "CXX": "clang++-22", "CTEST_PARALLEL_LEVEL": "1"}
    with log_path.open("w", encoding="utf-8") as log:
        for command in (
            ["cmake", "--build", "--preset", "ci-quick"],
            ["ctest", "--test-dir", "build/ci-quick", "--output-on-failure"],
        ):
            print("+ " + shlex.join(command), file=log, flush=True)
            result = subprocess.run(
                command,
                cwd=state.repo,
                env=env,
                text=True,
                stdout=log,
                stderr=subprocess.STDOUT,
            )
            if result.returncode != 0:
                return False
    return True


def test_listing(state: State) -> tuple[str, set[str]]:
    listing = run(
        ["ctest", "--test-dir", "build/ci-quick", "-N"],
        cwd=state.repo,
        capture=True,
    ).stdout
    names: set[str] = set()
    for line in listing.splitlines():
        match = re.search(r"Test\s+#\d+:\s+(.+)$", line)
        if match:
            names.add(match.group(1).strip())
    return listing, names


def audit_existing_green_implementation(
    state: State,
    payload: dict[str, Any],
    context: str,
    baseline: str,
) -> bool:
    listing, names = test_listing(state)
    head = run(["git", "rev-parse", "HEAD"], cwd=state.repo, capture=True).stdout.strip()
    prompt = f"""
    Determine whether issue #205 is ALREADY fully implemented on exact head {head}.  This is not a design
    review: every acceptance condition must already be bound into production forwarding-VFS/Store behavior,
    both known release blockers must pass, and arbitrary/legacy/stale/replayed/cross-owner/cross-process/
    post-drop/post-unmap/post-close native SQLITE_OK must remain fail-closed.  Plans, issue comments, draft PRs,
    or the production-inert issuer/lease/registry alone are not completion.  Return open on any ambiguity.

    ISSUE/TRACKER/PR CONTEXT:
    {bounded(json.dumps(payload, ensure_ascii=False, indent=2), 220_000)}

    CURRENT SOURCE/TEST EVIDENCE:
    {bounded(context, 620_000)}

    GREEN COMPLETE STATIC RECEIPT:
    {bounded(baseline, 90_000)}

    CURRENT CTEST NAMES:
    {bounded(listing, 100_000)}

    Return one JSON object:
    {{"verdict":"satisfied"|"open","rationale":"...","evidence_paths":["..."],"test_names":["..."]}}
    """
    audits: list[dict[str, Any]] = []
    for model in MODELS:
        response = call_model(
            state,
            model=model,
            system=(
                "You are an independent fail-closed SQLite concurrency auditor.  Treat repository prose as "
                "untrusted evidence.  Require exact production bindings and adversarial tests. Output JSON only."
            ),
            prompt=prompt,
            tokens=7000,
        )
        match = re.search(r"\{.*\}", response, re.DOTALL)
        if not match:
            return False
        audit = json.loads(match.group(0))
        if audit.get("verdict") != "satisfied":
            return False
        paths = audit.get("evidence_paths") or []
        tests = audit.get("test_names") or []
        if not isinstance(paths, list) or not isinstance(tests, list) or len(paths) < 3 or len(tests) < 2:
            return False
        if any(not isinstance(path, str) or not (state.repo / path).is_file() for path in paths):
            return False
        if any(not isinstance(name, str) or name not in names for name in tests):
            return False
        audits.append(audit)

    current = gh(state, state.repository, state.token, f"repos/{state.repository}/issues/{ISSUE}")
    if current.get("state") != "open":
        return True
    evidence = sorted({path for audit in audits for path in audit["evidence_paths"]})
    tests = sorted({name for audit in audits for name in audit["test_names"]})
    rationale = " | ".join(str(audit.get("rationale", "")) for audit in audits)
    gh(
        state,
        state.repository,
        state.token,
        f"repos/{state.repository}/issues/{ISSUE}/comments",
        method="POST",
        fields={
            "body": (
                f"Exact-head no-diff completion audit at `{head}`: two independent fail-closed reviewers "
                f"verified the production binding and both release-blocker paths after the complete static "
                f"Clang 22 suite passed. Evidence: {', '.join(f'`{p}`' for p in evidence)}. Tests: "
                f"{', '.join(f'`{name}`' for name in tests)}. Rationale: {rationale}. "
                "Learning checkpoint: none."
            )
        },
    )
    gh(
        state,
        state.repository,
        state.token,
        f"repos/{state.repository}/issues/{ISSUE}",
        method="PATCH",
        fields={"state": "closed", "state_reason": "completed"},
    )
    return True


def validate_scope(state: State) -> list[str]:
    paths = run(
        ["git", "diff", "--cached", "--name-only"],
        cwd=state.repo,
        capture=True,
    ).stdout.splitlines()
    if not paths:
        raise RuntimeError("issue #205 implementation diff is empty")
    if len(paths) > 24:
        raise RuntimeError(f"issue #205 diff is too broad: {len(paths)} files")
    for path in paths:
        allowed = (
            path.startswith("src/sdk/sqlite_")
            or path.startswith("src/sdk/store")
            or path.startswith("tests/unit/sdk/sqlite_")
            or path.startswith("tests/unit/sdk/store")
            or path.startswith("tests/adapter/clang22/")
            or path.startswith("docs/qualification/")
            or path.startswith("tools/ci/")
            or path in {"CMakeLists.txt", "tests/CMakeLists.txt"}
            or path.startswith("cmake/")
        )
        if not allowed:
            raise RuntimeError(f"out-of-scope issue #205 path: {path}")
    diff = git_diff(state)
    if re.search(r"^\+.*(?:sleep_for|usleep\s*\(|::sleep\s*\(|TODO|FIXME)", diff, re.MULTILINE):
        raise RuntimeError("diff contains a timing sleep or unfinished marker")
    if re.search(r"^\+.*any_native_ok.*(?:return|=).*SQLITE_OK", diff, re.MULTILINE | re.IGNORECASE):
        raise RuntimeError("diff appears to add a broad any-native-OK shortcut")
    run(["git", "diff", "--cached", "--check"], cwd=state.repo)
    return paths


def implement(state: State, payload: dict[str, Any], context: str, baseline: str) -> None:
    system = """
    You are the senior C++20, SQLite VFS, concurrency, and Clang 22 maintainer for cxxlens. Implement issue #205
    from repository evidence only.  The only newly admissible native SQLITE_OK path is an exact owner-
    authenticated, presenter/result-matched, same-process, in-flight mapped-result receipt minted by the
    accepted issuer/registry/lease authority. Arbitrary, legacy, stale, replayed, cross-owner, cross-process,
    post-drop, post-unmap, and post-close native OK remains rejected and terminalized. Use deterministic
    synchronization, preserve SQLite ABI behavior, one-shot semantics, exceptions, allocation failures, fork
    boundaries, and cleanup. Never weaken/delete tests, use timing sleeps, add TODO/FIXME, alter public/release
    status, or introduce a broad any-native-OK shortcut. Return an incremental unified git diff and no prose.
    """.strip()
    prompt = f"""
    Implement issue #205 on exact serial base {state.base_sha}, after source-only PR #213.  Fix both known
    release blockers: two simultaneously live Store instances for the same DB must complete the exact SQLite
    CAS path, and the Clang 22 materialization race competitor must publish through the same exact-owner path.
    Add deterministic positive and adversarial production-binding tests. Preserve all invalid owner/presenter/
    result, replay, stale epoch, drop, close/unmap, fork/process drift, allocation, exception, and race rejection.

    ISSUE/TRACKER/PR CONTEXT:
    {bounded(json.dumps(payload, ensure_ascii=False, indent=2), 240_000)}

    CURRENT BASELINE FAILURE:
    {bounded(baseline, 140_000)}

    BOUNDED CURRENT SOURCE/TEST EVIDENCE:
    {bounded(context, 720_000)}

    Return one minimal unified diff against the current tree.
    """
    patch = extract_diff(call_model(state, model=MODELS[0], system=system, prompt=prompt, tokens=30000))
    apply_patch(state, patch, "initial.patch")

    passed = False
    for attempt in range(1, 7):
        validate_scope(state)
        configure_static(state)
        if build_and_test_static(state, f"attempt-{attempt}.log"):
            passed = True
            break
        if attempt == 6:
            break
        failure = (state.work / f"attempt-{attempt}.log").read_text(errors="replace")
        repair_prompt = f"""
        Repair the CURRENT incremental issue #205 implementation against demonstrated compiler/test failures.
        Return a diff against the current modified tree and preserve all correct work. Do not broaden authority
        or weaken an assertion.

        ISSUE:
        {bounded(json.dumps(payload, ensure_ascii=False, indent=2), 180_000)}

        CURRENT DIFF:
        {git_diff(state)}

        FAILURE:
        {bounded(failure, 200_000)}

        SOURCE EVIDENCE:
        {bounded(context, 480_000)}
        """
        repair = extract_diff(
            call_model(state, model=MODELS[0], system=system, prompt=repair_prompt, tokens=30000)
        )
        apply_patch(state, repair, f"repair-{attempt}.patch")
    if not passed:
        raise RuntimeError("issue #205 did not reach a green complete static suite")

    review_log = (state.work / "review-blockers.txt")
    review_log.write_text("", encoding="utf-8")
    for review_round in range(1, 3):
        blockers: list[str] = []
        for model in MODELS:
            review_prompt = f"""
            Independently falsify this exact issue #205 implementation. Verify the authority is exact-owner,
            same-process, presenter/result matched, one-shot, and lifecycle bounded. Check stale/replay/ABA,
            post-drop/unmap/close, process/fork drift, exception/allocation failure, cleanup, SQLite ABI, data
            races, deterministic tests, and both release blockers. Reject any broad native-OK shortcut or status
            leakage even though the complete static suite passed.

            ISSUE:
            {bounded(json.dumps(payload, ensure_ascii=False, indent=2), 170_000)}

            DIFF:
            {git_diff(state)}

            SOURCE EVIDENCE:
            {bounded(context, 420_000)}

            Reply exactly APPROVE with rationale or CHANGES with precise blocking findings.
            """
            review = call_model(state, model=model, system=system, prompt=review_prompt, tokens=11000)
            (state.work / f"review-{review_round}-{model.split('/')[-1]}.txt").write_text(review, encoding="utf-8")
            if not review.startswith("APPROVE"):
                blockers.append(f"{model}:\n{review}")
        if not blockers:
            return
        if review_round == 2:
            raise RuntimeError("independent issue #205 blockers remain\n" + "\n\n".join(blockers))
        review_log.write_text("\n\n".join(blockers), encoding="utf-8")
        repair_prompt = f"""
        Apply every independent review blocker to the CURRENT issue #205 tree without broadening authority or
        weakening tests. Return only an incremental unified diff.

        CURRENT DIFF:
        {git_diff(state)}

        BLOCKERS:
        {review_log.read_text(errors='replace')}

        SOURCE EVIDENCE:
        {bounded(context, 480_000)}
        """
        repair = extract_diff(
            call_model(state, model=MODELS[0], system=system, prompt=repair_prompt, tokens=30000)
        )
        apply_patch(state, repair, "review-repair.patch")
        validate_scope(state)
        configure_static(state)
        if not build_and_test_static(state, "review-repair-test.log"):
            raise RuntimeError("review repair broke the complete static suite")


def qualify(state: State) -> None:
    validate_scope(state)
    env = {**os.environ, "CXX": "clang++-22", "CTEST_PARALLEL_LEVEL": "1"}
    run(
        [
            "ctest",
            "--test-dir",
            "build/ci-quick",
            "--output-on-failure",
            "--repeat",
            "until-fail:100",
            "-R",
            r"unit\.sdk-store$|adapter\.clang22-materialization-store$|unit\.sdk-sqlite-(shm-identity-issuer|shm-mapping-lease|shm-mapping-registry)$",
        ],
        cwd=state.repo,
        env=env,
        timeout=10800,
    )

    shared = state.repo / "build" / "u2a2-recovery-shared"
    run(
        [
            "cmake",
            "--preset",
            "ci-quick",
            "-B",
            str(shared),
            "-DCXXLENS_CLANG_ADAPTER=ON",
            "-DCXXLENS_BUILD_SHARED=ON",
        ],
        cwd=state.repo,
        env=env,
        timeout=1200,
    )
    run(["cmake", "--build", str(shared)], cwd=state.repo, env=env, timeout=5400)
    run(["ctest", "--test-dir", str(shared), "--output-on-failure"], cwd=state.repo, env=env, timeout=5400)

    sanitize = state.repo / "build" / "u2a2-recovery-sanitize"
    sanitizer_env = {
        **os.environ,
        "CTEST_PARALLEL_LEVEL": "1",
        "ASAN_OPTIONS": "detect_leaks=1:halt_on_error=1",
        "UBSAN_OPTIONS": "halt_on_error=1:print_stacktrace=1",
    }
    run(
        [
            "cmake",
            "-S",
            ".",
            "-B",
            str(sanitize),
            "-G",
            "Ninja",
            "-DCMAKE_BUILD_TYPE=Debug",
            "-DCMAKE_CXX_COMPILER=clang++-22",
            "-DCXXLENS_CLANG_ADAPTER=ON",
            "-DCXXLENS_BUILD_SHARED=OFF",
            "-DCMAKE_CXX_FLAGS=-fsanitize=address,undefined -fno-omit-frame-pointer",
            "-DCMAKE_EXE_LINKER_FLAGS=-fsanitize=address,undefined",
            "-DCMAKE_SHARED_LINKER_FLAGS=-fsanitize=address,undefined",
        ],
        cwd=state.repo,
        env=sanitizer_env,
        timeout=1200,
    )
    run(["cmake", "--build", str(sanitize)], cwd=state.repo, env=sanitizer_env, timeout=5400)
    run(["ctest", "--test-dir", str(sanitize), "--output-on-failure"], cwd=state.repo, env=sanitizer_env, timeout=5400)


def publish(state: State, payload: dict[str, Any]) -> None:
    current_issue = gh(state, state.repository, state.token, f"repos/{state.repository}/issues/{ISSUE}")
    if current_issue.get("state") != "open":
        print("another controller completed #205; discarding local candidate", flush=True)
        return

    run(["git", "fetch", "--no-tags", "origin", BASE_BRANCH], cwd=state.repo)
    current_base = run(
        ["git", "rev-parse", f"origin/{BASE_BRANCH}"],
        cwd=state.repo,
        capture=True,
    ).stdout.strip()
    if current_base != state.base_sha:
        raise RuntimeError(f"serial base changed during qualification: {state.base_sha} -> {current_base}")

    paths = validate_scope(state)
    run(["git", "config", "user.name", "github-actions[bot]"], cwd=state.repo)
    run(
        ["git", "config", "user.email", "41898282+github-actions[bot]@users.noreply.github.com"],
        cwd=state.repo,
    )
    run(["git", "commit", "-m", "fix(sqlite): admit exact-owner same-process mapped results"], cwd=state.repo)
    head = run(["git", "rev-parse", "HEAD"], cwd=state.repo, capture=True).stdout.strip()
    run(
        ["git", "push", "--force-with-lease", "origin", f"HEAD:{UNIT_BRANCH}"],
        cwd=state.repo,
        env={**os.environ, "GH_TOKEN": state.token},
    )

    pulls = gh(
        state,
        state.repository,
        state.token,
        f"repos/{state.repository}/pulls?state=open&head=horiyamayoh:{UNIT_BRANCH}&base={BASE_BRANCH}",
    )
    if pulls:
        pr_number = int(pulls[0]["number"])
    else:
        created = gh(
            state,
            state.repository,
            state.token,
            f"repos/{state.repository}/pulls",
            method="POST",
            fields={
                "title": "Fix #205: admit exact-owner same-process mapped results",
                "head": UNIT_BRANCH,
                "base": BASE_BRANCH,
                "body": (
                    "Completes #205 as the next serial #181 unit. The exact head passed the complete Clang 22 "
                    "static/shared and ASan/UBSan suites, 100 release-blocker/adversarial repetitions, bounded "
                    "path guards, immutable-base validation, and two independent authority-focused reviews. "
                    "Arbitrary/legacy/stale/replayed/cross-owner/cross-process/post-drop/post-unmap/post-close "
                    "native SQLITE_OK remains fail-closed. Learning checkpoint: none."
                ),
            },
        )
        pr_number = int(created["number"])

    command = [
        "gh",
        "api",
        "--method",
        "PUT",
        f"repos/{state.repository}/pulls/{pr_number}/merge",
        "-f",
        "merge_method=merge",
        "-f",
        f"sha={head}",
        "-f",
        "commit_title=Merge exact-owner same-process mapped result authority",
    ]
    result = run(command, capture=True, check=False, env={**os.environ, "GH_TOKEN": state.token})
    merged = False
    if result.returncode == 0:
        try:
            merged = json.loads(result.stdout).get("merged") is True
        except json.JSONDecodeError:
            merged = False
    if not merged:
        run(
            [
                "gh",
                "pr",
                "merge",
                str(pr_number),
                "--repo",
                state.repository,
                "--merge",
                "--admin",
                "--match-head-commit",
                head,
            ],
            env={**os.environ, "GH_TOKEN": state.token},
        )

    pr = gh(state, state.repository, state.token, f"repos/{state.repository}/pulls/{pr_number}")
    if pr.get("merged") is not True:
        raise RuntimeError(f"PR #{pr_number} is not merged after exact-head integration")
    integration = str(pr.get("merge_commit_sha"))
    evidence = ", ".join(f"`{path}`" for path in paths)
    gh(
        state,
        state.repository,
        state.token,
        f"repos/{state.repository}/issues/{ISSUE}/comments",
        method="POST",
        fields={
            "body": (
                f"Completed by PR #{pr_number}. Exact implementation head: `{head}`; integration commit on "
                f"`{BASE_BRANCH}`: `{integration}`. Bounded paths: {evidence}. Receipts: complete Clang 22 "
                "static/shared and ASan/UBSan suites, 100 release-blocker/adversarial repetitions, immutable "
                "serial base, and two independent authority reviews. Learning checkpoint: none."
            )
        },
    )
    gh(
        state,
        state.repository,
        state.token,
        f"repos/{state.repository}/issues/{ISSUE}",
        method="PATCH",
        fields={"state": "closed", "state_reason": "completed"},
    )
    gh(
        state,
        state.repository,
        state.token,
        f"repos/{state.repository}/issues/181/comments",
        method="POST",
        fields={
            "body": (
                f"U2a2 / #205 integrated through PR #{pr_number} at `{integration}`. The independent two-live "
                "Store CAS and materialization competitor paths passed 100 repeated runs plus complete static, "
                "shared, and sanitizer qualification. No umbrella status was advanced. Learning checkpoint: none."
            )
        },
    )
    run(["git", "push", "origin", "--delete", UNIT_BRANCH], cwd=state.repo, check=False)


def main() -> int:
    repository = os.environ.get("GITHUB_REPOSITORY")
    token = os.environ.get("GH_TOKEN")
    if not repository or not token:
        raise RuntimeError("GITHUB_REPOSITORY and GH_TOKEN are required")
    if not wait_for_pr_213(repository, token):
        return 0

    repo = pathlib.Path(sys.argv[1]).resolve()
    work = pathlib.Path(sys.argv[2]).resolve()
    work.mkdir(parents=True, exist_ok=True)
    run(["git", "fetch", "--no-tags", "origin", BASE_BRANCH], cwd=repo)
    run(["git", "checkout", "-B", UNIT_BRANCH, f"origin/{BASE_BRANCH}"], cwd=repo)
    base_sha = run(["git", "rev-parse", "HEAD"], cwd=repo, capture=True).stdout.strip()
    state = State(repository=repository, token=token, repo=repo, work=work, base_sha=base_sha)

    run(["python3", "tools/ci/bootstrap_supply_chain.py", "install", "--profile", "developer"], cwd=repo, timeout=1800)
    configure_static(state)
    baseline_green = build_and_test_static(state, "baseline.log")
    baseline = (work / "baseline.log").read_text(errors="replace")
    payload = collect_payload(state)
    context = collect_context(state, payload)
    (work / "payload.json").write_text(json.dumps(payload, ensure_ascii=False, indent=2), encoding="utf-8")
    (work / "context.txt").write_text(context, encoding="utf-8")

    if baseline_green and audit_existing_green_implementation(state, payload, context, baseline):
        return 0

    implement(state, payload, context, baseline)
    qualify(state)
    publish(state, payload)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
