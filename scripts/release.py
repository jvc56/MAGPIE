#!/usr/bin/env python3
"""Releases MAGPIE for Windows, macOS and Linux.

    python3 scripts/release.py [--dry-run] [--version X.Y.Z] [--notes-file FILE]

Everything happens against origin/main, in a temporary worktree, so the branch
and working tree this runs from do not matter. The questions all come first,
so that it can be left alone afterwards:

 1. Lint origin/main and stop if its CI has failed.
 2. Ask for the version, if the one on main is not newer than the latest
    release. It is always newer than every release and every tag.
 3. Draft the release notes and ask for feedback until they are accepted.
 4. Save them as a draft release, which nobody else can see.
 5. Open a PR that bumps MAGPIE_VERSION, wait for CI on it, and merge it. The
    bump commit is what gets tagged: CI ran on exactly that commit and the
    notes describe exactly its history, however far main moves meanwhile. If
    main already has the version, its head is tagged instead, after CI.
 6. Push the tag. release-{windows,macos,linux}.yml build and test on every
    architecture, then upload to the draft.
 7. Check that every file arrived, then publish the draft as the latest
    release. Until then nothing is public, so one platform failing leaves
    nothing half released.

A failed workflow pauses to offer rerunning its failed jobs. To resume after
any stop, including Ctrl-C, run this again: it works out what is left from the
draft, the PR and the tag on GitHub.

A rerun cannot fix a failure in the code, or in a workflow: both come from the
tagged commit. So fix main, and start again from it, keeping the accepted
notes and the version; tags are never moved, so the tag goes with the draft:

    gh release view vX.Y.Z --json body -q .body > notes.md
    gh release delete vX.Y.Z --yes --cleanup-tag && git tag -d vX.Y.Z
    python3 scripts/release.py --notes-file notes.md

The checks are CI's: the whole of run-tests.yml, test-wasm.yml and
test-xs-codecs.yml on the commit, then the tests and first-run smoke tests in
the release workflows, on the release executables. CI pins the tool versions
that decide, so only its quick lints run here, to fail in seconds rather than
half an hour.

Needs git and an authenticated gh. The notes are drafted and revised with the
`claude` CLI when there is one; without it they are edited by hand in $EDITOR.
"""

import argparse
import contextlib
import glob
import importlib.util
import json
import os
import re
import shlex
import shutil
import subprocess
import sys
import tempfile
import time
from typing import Dict, Iterable, List, Optional, Sequence, Tuple

REPO_ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
REMOTE = "origin"
MAIN = "main"
VERSION_FILE = "src/impl/config.c"
VERSION_DEFINE = re.compile(r'^#define MAGPIE_VERSION "(.*)"$', re.MULTILINE)
# Exactly three numbers: the Makefile turns them into Windows' FILEVERSION.
VERSION_TEXT = re.compile(r"\d+\.\d+\.\d+")
RELEASE_TAG = re.compile(r"v(\d+\.\d+\.\d+)")
COMMIT_HASH = re.compile(r"[0-9a-f]{40}")

CI_WORKFLOWS = ("ci", "Test WASM Build", "Test XS codecs")
RELEASE_WORKFLOWS = ("Release Windows", "Release macOS", "Release Linux")
PASSED_CONCLUSIONS = ("success", "skipped", "neutral")

POLL_SECONDS = 60
HEARTBEAT_SECONDS = 15 * 60
# How long a pushed commit's workflow runs get to appear, and how many polls in
# a row may fail, before a wait gives up. An hours-long wait should outlast a
# network blip, and if it does not, running this again resumes it.
RUNS_APPEAR_SECONDS = 15 * 60
MAX_POLL_FAILURES = 30
CLAUDE_TIMEOUT_SECONDS = 5 * 60

CLAUDE_INSTRUCTIONS = """\
You edit the release notes of MAGPIE, a crossword game engine, for the people
who download and use it. Reply with only the complete notes in GitHub
Markdown: no preamble, no code fence around them. Use only facts in the text
below. Keep the Downloads section word for word unless told to change it.
"""
CLAUDE_DRAFT_REQUEST = """\
Rewrite the "What's new" section. Replace its pull request list with a few
short bullets per theme on what changes for someone using MAGPIE, keeping the
(#number) references and the Full Changelog link. Lead with what matters most.
Leave out work on CI, tests, notes and internals that no user would notice.
"""


class ReleaseError(Exception):
    """A reason to stop that the message explains; no traceback needed."""


def step(message: str) -> None:
    print(f"\n==> {message}", flush=True)


def run(*command: str, cwd: Optional[str] = None) -> str:
    """Runs a command and returns its output, stripped; failure stops it."""
    result = subprocess.run(
        command,
        cwd=cwd or REPO_ROOT,
        stdin=subprocess.DEVNULL,
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
        universal_newlines=True,
    )
    if result.returncode != 0:
        detail = (result.stderr or result.stdout).strip()
        raise ReleaseError(f"{' '.join(command)} failed: {detail}")
    return result.stdout.strip()


def gh_json(*arguments: str):
    return json.loads(run("gh", *arguments) or "null")


def ask(question: str) -> str:
    try:
        return input(f"{question} ").strip()
    except EOFError:
        raise ReleaseError(
            f"no terminal to answer this on: {question}\n"
            "--version and --notes-file answer the two usual questions."
        ) from None


def version_key(version: str) -> Tuple[int, ...]:
    return tuple(int(part) for part in version.split("."))


def tag_of(version: str) -> str:
    return f"v{version}"


def release_files(version: str) -> List[str]:
    """Every file the three release workflows attach to a release."""
    files = [
        f"magpie-{version}-windows-setup.exe",
        f"magpie-{version}-windows-x64.exe",
        f"magpie-{version}-windows-arm64.exe",
        f"magpie-{version}-macos.pkg",
        f"magpie-{version}-macos-universal.zip",
        f"magpie-{version}-linux-x64.tar.gz",
        f"magpie-{version}-linux-arm64.tar.gz",
    ]
    return files + [f"{file_name}.sha256" for file_name in files]


def remote_tags() -> Dict[str, str]:
    """Maps each vX.Y.Z tag on the remote to the commit it names."""
    listing = run("git", "ls-remote", "--tags", REMOTE, "v*")
    tags: Dict[str, str] = {}
    # An annotated tag has a second line, name^{}, for its commit; it sorts
    # after the first line and replaces the tag object's hash.
    for line in listing.splitlines():
        commit, reference = line.split()
        name = reference[len("refs/tags/") :].replace("^{}", "")
        if RELEASE_TAG.fullmatch(name):
            tags[name] = commit
    return tags


def read_version(commit: str) -> str:
    source = run("git", "show", f"{commit}:{VERSION_FILE}")
    match = VERSION_DEFINE.search(source)
    if match is None or VERSION_TEXT.fullmatch(match.group(1)) is None:
        raise ReleaseError(f"no X.Y.Z MAGPIE_VERSION in {VERSION_FILE}")
    return match.group(1)


def write_version(worktree: str, version: str) -> None:
    path = os.path.join(worktree, VERSION_FILE)
    with open(path, "rb") as source:
        text = source.read().decode("utf-8")
    text, replacements = VERSION_DEFINE.subn(
        f'#define MAGPIE_VERSION "{version}"', text
    )
    if replacements != 1:
        raise ReleaseError(f"expected one MAGPIE_VERSION in {VERSION_FILE}")
    with open(path, "wb") as target:
        target.write(text.encode("utf-8"))


@contextlib.contextmanager
def worktree_at(commit: str):
    """A temporary checkout of a commit, leaving this one untouched."""
    parent = tempfile.mkdtemp(prefix="magpie-release-")
    worktree = os.path.join(parent, "worktree")
    run("git", "worktree", "add", "--quiet", "--detach", worktree, commit)
    try:
        yield worktree
    finally:
        with contextlib.suppress(ReleaseError):
            run("git", "worktree", "remove", "--force", worktree)
        shutil.rmtree(parent, ignore_errors=True)


def next_versions(latest: str) -> Dict[str, str]:
    major, minor, patch = version_key(latest)
    return {
        "patch": f"{major}.{minor}.{patch + 1}",
        "minor": f"{major}.{minor + 1}.0",
        "major": f"{major + 1}.0.0",
    }


def choose_version(main_version: str, latest: str, requested: Optional[str]) -> str:
    def problem_with(version: str) -> Optional[str]:
        if VERSION_TEXT.fullmatch(version) is None:
            return f"{version} is not of the form X.Y.Z"
        if version_key(version) <= version_key(latest):
            return f"{version} is not newer than the latest release, {latest}"
        if version_key(version) < version_key(main_version):
            return f"{version} is older than {main_version}, the version on {MAIN}"
        return None

    if requested is not None:
        problem = problem_with(requested)
        if problem is not None:
            raise ReleaseError(problem)
        return requested
    if version_key(main_version) > version_key(latest):
        print(f"{MAIN} is at {main_version}, newer than the latest release, {latest}.")
        return main_version

    print(f"{MAIN} is at {main_version}; the latest release is {latest}.")
    options = next_versions(latest)
    choices = ", ".join(f"{name} = {value}" for name, value in options.items())
    while True:
        answer = ask(f"New version ({choices}; Enter for patch, or X.Y.Z):")
        version = options.get(answer or "patch", answer)
        problem = problem_with(version)
        if problem is None:
            return version
        print(problem)


def run_local_checks(worktree: str) -> None:
    """CI's quick lints. A missing tool only skips one: CI will run it."""
    checks = (
        ("formatting", "scripts/format.py", shutil.which("clang-format-20")),
        ("portability", "scripts/check_portability.py", True),
        (
            "circular dependencies",
            "scripts/find_circ_deps.py",
            importlib.util.find_spec("networkx"),
        ),
    )
    for name, script, available in checks:
        if not available:
            print(f"  {name}: skipped, its tool is not installed; CI will check it")
            continue
        try:
            run(sys.executable, script, cwd=worktree)
        except ReleaseError as error:
            raise ReleaseError(f"{name} failed on {MAIN}.\n{error}") from None
        print(f"  {name}: ok")


def changes_since(previous_tag: str, tag: str, commit: str) -> str:
    """The pull requests since a tag, as (#number) bullets and a compare link."""
    generated = run(
        "gh",
        "api",
        "repos/{owner}/{repo}/releases/generate-notes",
        "-f",
        f"tag_name={tag}",
        "-f",
        f"target_commitish={commit}",
        "-f",
        f"previous_tag_name={previous_tag}",
        "--jq",
        ".body",
    )
    lines = []
    for line in generated.splitlines():
        if line.startswith("**Full Changelog**"):
            lines += ["", line]
        elif line.startswith("* ") and " by @" in line:
            lines.append(re.sub(r" by @\S+ in \S+/pull/(\d+)$", r" (#\1)", line))
    return "\n".join(lines)


def template_notes(version: str, changes: str) -> str:
    return f"""\
MAGPIE {version} for Windows, macOS and Linux.

## What's new

{changes}

## Downloads

| System | Installer | Single executable |
| --- | --- | --- |
| Windows x64 or ARM | `magpie-{version}-windows-setup.exe` | `magpie-{version}-windows-x64.exe`, `magpie-{version}-windows-arm64.exe` |
| macOS, Apple silicon or Intel | `magpie-{version}-macos.pkg` | `magpie-{version}-macos-universal.zip` |
| Linux x64 or ARM | | `magpie-{version}-linux-x64.tar.gz`, `magpie-{version}-linux-arm64.tar.gz` |

Each file has a `.sha256` beside it. On first run, MAGPIE offers to download its data (about 195 MB).

The files are not code-signed, so Windows and macOS ask you to confirm the first run. The README's Windows, macOS and Linux sections have the steps.
"""


def find_claude() -> Optional[str]:
    """The claude CLI, or the one in a VS Code extension if not on the PATH."""
    on_path = shutil.which("claude")
    if on_path is not None:
        return on_path
    pattern = "~/.vscode*/extensions/anthropic.claude-code-*/resources/native-binary/claude"
    installed = glob.glob(os.path.expanduser(pattern))
    return max(installed, key=os.path.getmtime) if installed else None


def ask_claude(claude: str, request: str, notes: str) -> Optional[str]:
    """Claude's edit of the notes, without tools; None if that did not work."""
    prompt = f"{CLAUDE_INSTRUCTIONS}\n{request}\nThe notes:\n\n{notes}"
    try:
        result = subprocess.run(
            [claude, "-p", "--tools", "", "--no-session-persistence"],
            input=prompt,
            cwd=tempfile.gettempdir(),
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
            universal_newlines=True,
            timeout=CLAUDE_TIMEOUT_SECONDS,
        )
    except (OSError, subprocess.TimeoutExpired) as error:
        print(f"claude did not finish: {error}")
        return None
    edited = result.stdout.strip()
    if result.returncode != 0 or not edited:
        print(f"claude failed: {result.stderr.strip() or result.stdout.strip()}")
        return None
    return edited + "\n"


def edit_by_hand(notes: str) -> str:
    editor = os.environ.get("VISUAL") or os.environ.get("EDITOR") or "vi"
    with tempfile.NamedTemporaryFile(
        "w", suffix=".md", prefix="magpie-notes-", delete=False
    ) as draft:
        draft.write(notes)
    try:
        subprocess.run(shlex.split(editor) + [draft.name], check=True)
        with open(draft.name) as edited:
            return edited.read()
    except (OSError, subprocess.CalledProcessError) as error:
        print(f"{editor} failed, so the notes are unchanged: {error}")
        return notes
    finally:
        os.unlink(draft.name)


def review_notes(notes: str, claude: Optional[str], dry_run: bool) -> str:
    """Shows the notes until they are accepted, and returns those."""
    while True:
        print(f"\n{'-' * 72}\n{notes.rstrip()}\n{'-' * 72}")
        accept = "accept them" if dry_run else "accept them and start the release"
        feedback_hint = (
            ", or type what to change"
            if claude
            else " (no claude CLI, so feedback goes through e)"
        )
        answer = ask(f"[a] {accept}, [e] edit, [q] quit{feedback_hint}:")
        choice = answer.lower()
        if choice in ("a", "accept"):
            return notes
        if choice in ("q", "quit"):
            raise ReleaseError("stopped before changing anything")
        if choice in ("e", "edit"):
            notes = edit_by_hand(notes)
        elif answer and claude:
            print("Revising...", flush=True)
            notes = ask_claude(claude, f"Apply this feedback: {answer}", notes) or notes
        elif answer:
            print("There is no claude CLI to revise them; [e] edits them by hand.")


def draft_notes(version: str, latest: str, commit: str, dry_run: bool) -> str:
    step("Drafting the release notes")
    notes = template_notes(version, changes_since(tag_of(latest), tag_of(version), commit))
    claude = find_claude()
    if claude is None:
        print("No claude CLI found: this is the plain list of pull requests.")
    else:
        print("Summarizing the pull requests with claude...", flush=True)
        notes = ask_claude(claude, CLAUDE_DRAFT_REQUEST, notes) or notes
    return review_notes(notes, claude, dry_run)


def create_draft(tag: str, notes: str, commit: Optional[str]) -> None:
    """Saves the notes, and the commit to tag if it is known already."""
    target = ["--target", commit] if commit else []
    with tempfile.NamedTemporaryFile("w", suffix=".md", delete=False) as notes_file:
        notes_file.write(notes)
    try:
        run(
            "gh", "release", "create", tag, "--draft", "--title", tag,
            "--notes-file", notes_file.name, *target,
        )
    finally:
        os.unlink(notes_file.name)


def list_runs(branch: str, commit: str) -> List[dict]:
    return gh_json(
        "run", "list", "--branch", branch, "--commit", commit, "--limit", "50",
        "--json", "databaseId,workflowName,status,conclusion,url",
    )


def failed_runs(runs: Iterable[dict]) -> List[dict]:
    return [
        workflow_run
        for workflow_run in runs
        if workflow_run["status"] == "completed"
        and workflow_run["conclusion"] not in PASSED_CONCLUSIONS
    ]


def offer_rerun(failures: Sequence[dict]) -> None:
    """Reports failed workflows; returns once their failed jobs restarted."""
    print()
    for failure in failures:
        print(f"{failure['workflowName']} failed: {failure['url']}")
        with contextlib.suppress(ReleaseError):
            jobs = gh_json(
                "run", "view", str(failure["databaseId"]), "--json", "jobs"
            )["jobs"]
            for job in failed_runs(jobs):
                print(f"  {job['name']}: {job['conclusion']}")
    while True:
        answer = ask("[r] rerun the failed jobs, [q] quit:").lower()
        if answer in ("q", "quit"):
            raise ReleaseError(
                "stopped on a failed workflow. Run this again to resume once it "
                "passes."
            )
        if answer in ("r", "rerun"):
            for failure in failures:
                run("gh", "run", "rerun", str(failure["databaseId"]), "--failed")
            return


def wait_for_workflows(branch: str, commit: str, required: Sequence[str]) -> None:
    """Waits for every workflow run of a commit on a branch or tag to pass."""
    start = time.monotonic()
    last_heartbeat = start
    reported: Dict[int, str] = {}
    poll_failures = 0
    while True:
        try:
            runs = list_runs(branch, commit)
        except ReleaseError as error:
            poll_failures += 1
            if poll_failures >= MAX_POLL_FAILURES:
                raise
            print(f"Could not check the runs, will try again: {error}")
            time.sleep(POLL_SECONDS)
            continue
        poll_failures = 0

        now = time.monotonic()
        elapsed = int(now - start) // 60
        for workflow_run in runs:
            state = workflow_run["conclusion"] or workflow_run["status"]
            if reported.get(workflow_run["databaseId"]) != state:
                reported[workflow_run["databaseId"]] = state
                print(f"[{elapsed:3d} min] {workflow_run['workflowName']}: {state}")

        names = {workflow_run["workflowName"] for workflow_run in runs}
        missing = [name for name in required if name not in names]
        unfinished = [
            workflow_run["workflowName"]
            for workflow_run in runs
            if workflow_run["status"] != "completed"
        ]
        failures = failed_runs(runs)
        if failures:
            offer_rerun(failures)
        elif not missing and not unfinished:
            return
        elif missing and now - start > RUNS_APPEAR_SECONDS:
            raise ReleaseError(
                f"no run of {', '.join(missing)} for {commit} on {branch}"
            )
        elif now - last_heartbeat >= HEARTBEAT_SECONDS:
            last_heartbeat = now
            print(f"[{elapsed:3d} min] waiting for {', '.join(unfinished + missing)}")
        time.sleep(POLL_SECONDS)


def open_version_pr(version: str, base_commit: str, branch: str) -> dict:
    with worktree_at(base_commit) as worktree:
        write_version(worktree, version)
        run("git", "commit", "--quiet", "-am", f"Version {version}", cwd=worktree)
        commit = run("git", "rev-parse", "HEAD", cwd=worktree)
        # The script's own branch: a closed attempt's copy can go.
        run(
            "git", "push", "--quiet", "--force", REMOTE,
            f"{commit}:refs/heads/{branch}", cwd=worktree,
        )
    url = run(
        "gh", "pr", "create", "--base", MAIN, "--head", branch,
        "--title", f"Version {version}",
        "--body", f"Bumps MAGPIE_VERSION for {tag_of(version)}, from scripts/release.py.",
    )
    print(f"Opened {url}")
    return {"url": url, "headRefOid": commit}


def land_version(
    version: str, main_commit: str, main_version: str, tags: Dict[str, str]
) -> str:
    """Gets the version onto main; returns the commit to tag, CI having passed.

    The draft's target records that commit once it is chosen, so that a resume
    tags the same one. Nothing else can say: main moves on, and a merged bump
    PR may belong to an attempt that was given up, whose commit lacks the fix
    that the attempt was given up for.
    """
    tag = tag_of(version)
    if tag in tags:
        return tags[tag]
    branch = f"release/{tag}"
    pull_requests = gh_json(
        "pr", "list", "--head", branch, "--state", "all",
        "--json", "state,headRefOid,url",
    )
    by_state = {pull_request["state"]: pull_request for pull_request in pull_requests}
    target = gh_json("release", "view", tag, "--json", "targetCommitish")[
        "targetCommitish"
    ]
    if "OPEN" not in by_state and COMMIT_HASH.fullmatch(target):
        if target == by_state.get("MERGED", {}).get("headRefOid"):
            return target
        step(f"Waiting for CI on {MAIN} (usually 15 to 40 minutes)")
        wait_for_workflows(MAIN, target, CI_WORKFLOWS)
        return target
    if "OPEN" not in by_state and main_version == version:
        run("gh", "release", "edit", tag, "--target", main_commit)
        step(f"Waiting for CI on {MAIN} (usually 15 to 40 minutes)")
        wait_for_workflows(MAIN, main_commit, CI_WORKFLOWS)
        return main_commit

    step(f"Bumping MAGPIE_VERSION to {version}")
    pull_request = by_state.get("OPEN") or open_version_pr(version, main_commit, branch)
    commit = pull_request["headRefOid"]
    step("Waiting for CI on the PR (usually 15 to 40 minutes)")
    wait_for_workflows(branch, commit, CI_WORKFLOWS)
    run("gh", "release", "edit", tag, "--target", commit)
    # A merge rather than a squash, so that the commit CI tested joins main.
    run(
        "gh", "pr", "merge", pull_request["url"], "--merge",
        "--match-head-commit", commit,
    )
    with contextlib.suppress(ReleaseError):
        run("git", "push", "--quiet", REMOTE, "--delete", branch)
    print(f"Merged {pull_request['url']}")
    return commit


def push_tag(version: str, commit: str) -> None:
    tag = tag_of(version)
    step(f"Tagging {commit[:8]} as {tag}")
    reference = f"refs/tags/{tag}"
    run("git", "fetch", "--quiet", REMOTE, MAIN)
    try:
        existing = run("git", "rev-parse", "--verify", "--quiet", f"{reference}^{{commit}}")
    except ReleaseError:
        existing = None
    if existing is None:
        run("git", "tag", "-a", tag, "-m", f"MAGPIE {version}", commit)
    elif existing != commit:
        raise ReleaseError(
            f"a local tag {tag} names {existing}, not {commit}. It is not on "
            f"{REMOTE}: remove it with `git tag -d {tag}` and run this again."
        )
    run("git", "push", "--quiet", REMOTE, reference)


def publish(version: str) -> str:
    tag = tag_of(version)
    release = gh_json("release", "view", tag, "--json", "assets,isDraft")
    if not release["isDraft"]:
        raise ReleaseError(f"{tag} is not a draft any more; nothing was published")
    uploaded = {asset["name"] for asset in release["assets"]}
    missing = sorted(set(release_files(version)) - uploaded)
    if missing:
        raise ReleaseError(
            f"the draft of {tag} lacks: {', '.join(missing)}\n"
            "Nothing was published. Add them, then run this again."
        )
    run(
        "gh", "release", "edit", tag, "--draft=false", "--prerelease=false",
        "--latest", "--verify-tag",
    )
    return gh_json("release", "view", tag, "--json", "url")["url"]


def parse_arguments() -> argparse.Namespace:
    parser = argparse.ArgumentParser(
        description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter
    )
    parser.add_argument(
        "--dry-run",
        action="store_true",
        help="ask the questions, then stop before changing anything",
    )
    parser.add_argument(
        "--version", help="the new version, X.Y.Z, rather than being asked"
    )
    parser.add_argument(
        "--notes-file", help="the accepted release notes, rather than being asked"
    )
    return parser.parse_args()


def release(arguments: argparse.Namespace) -> None:
    for tool in ("git", "gh"):
        if shutil.which(tool) is None:
            raise ReleaseError(f"{tool} is not installed")
    run("gh", "auth", "status")
    notes = None
    if arguments.notes_file:
        with open(arguments.notes_file) as notes_file:
            notes = notes_file.read()

    step(f"Reading {REMOTE}/{MAIN} and the releases")
    run("git", "fetch", "--quiet", REMOTE, MAIN)
    main_commit = run("git", "rev-parse", f"{REMOTE}/{MAIN}")
    main_version = read_version(main_commit)
    tags = remote_tags()
    releases = gh_json("release", "list", "--limit", "200", "--json", "tagName,isDraft")
    listed_tags = [
        listed for listed in releases if RELEASE_TAG.fullmatch(listed["tagName"])
    ]
    drafts = {listed["tagName"] for listed in listed_tags if listed["isDraft"]}
    # A tag counts unless it belongs to a draft: that is this script at work.
    finished = (set(tags) | {listed["tagName"] for listed in listed_tags}) - drafts
    latest = max((tag[1:] for tag in finished), key=version_key)
    # A draft newer than everything finished is a release this script began.
    resumable = [
        tag[1:] for tag in drafts if version_key(tag[1:]) > version_key(latest)
    ]
    if resumable:
        version = max(resumable, key=version_key)
        if arguments.version not in (None, version):
            raise ReleaseError(
                f"a draft of {tag_of(version)} is already under way. Delete it "
                f"with `gh release delete {tag_of(version)} --yes --cleanup-tag` "
                "to start over."
            )
        print(f"Resuming the draft of {tag_of(version)}, with its notes.")
        if arguments.dry_run:
            print("Dry run: stopping here.")
            return
    else:
        failures = failed_runs(list_runs(MAIN, main_commit))
        for failure in failures:
            print(f"{failure['workflowName']} failed on {MAIN}: {failure['url']}")
        if failures and ask("Release anyway? [y/N]").lower() not in ("y", "yes"):
            raise ReleaseError(f"stopped: {MAIN} is failing")
        with worktree_at(main_commit) as worktree:
            step("Linting")
            run_local_checks(worktree)
        step("Choosing the version")
        version = choose_version(main_version, latest, arguments.version)
        if notes is None:
            notes = draft_notes(version, latest, main_commit, arguments.dry_run)
        if arguments.dry_run:
            print(f"Dry run: {tag_of(version)} would start here. Nothing was changed.")
            return
        step(f"Saving the notes as a draft of {tag_of(version)}")
        create_draft(
            tag_of(version), notes, main_commit if main_version == version else None
        )

    print("Nothing below needs you unless a workflow fails.")
    commit = land_version(version, main_commit, main_version, tags)
    tag = tag_of(version)
    if tag not in tags:
        push_tag(version, commit)
    step("Waiting for the release workflows (usually one to five hours)")
    wait_for_workflows(tag, commit, RELEASE_WORKFLOWS)
    step(f"Publishing {tag}")
    url = publish(version)
    print(f"\nReleased {tag}: {url}")
    print(
        "If it changes what a birdtest task computes, also raise birdtest's "
        f"MIN_MAGPIE_VERSION and note why above MAGPIE_VERSION in {VERSION_FILE}."
    )


def main() -> int:
    try:
        release(parse_arguments())
    except ReleaseError as error:
        print(f"\nStopped: {error}", file=sys.stderr)
        return 1
    except KeyboardInterrupt:
        print("\nInterrupted. Run this again to resume.", file=sys.stderr)
        return 130
    return 0


if __name__ == "__main__":
    sys.exit(main())
