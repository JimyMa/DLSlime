# Contributing to DLSlime

Thank you for helping improve DLSlime. This guide describes how to propose,
implement, review, and merge changes. It applies to maintainers and external
contributors alike.

DLSlime follows a lightweight [GitHub Flow](https://docs.github.com/en/get-started/using-github/github-flow):
work from a short-lived branch, propose the change in a pull request (PR), and
merge it into `main` after review and validation.

## Before You Start

Search existing issues and PRs before starting work.

- Bug fixes and small, self-contained documentation changes may go directly to
  a PR. The PR must still explain the problem and how the change was validated.
- New features, public API changes, architectural changes, and substantial
  performance work should begin with an issue. Agree on the problem and scope
  with maintainers before investing in an implementation.
- Use an RFC or design issue when requirements or trade-offs are not yet clear.
  Record the decision before creating implementation tasks.
- Report security vulnerabilities privately to the maintainers rather than in
  a public issue.

An actionable issue should state the problem, scope, non-goals, acceptance
criteria, and validation plan. Large goals should be split into independently
reviewable tasks. A PR should close an issue only when it satisfies all of that
issue's acceptance criteria; otherwise, reference the issue without closing it.

## Branches and Commits

Create branches from the latest `main`. Keep each branch focused on one PR and
delete it after the PR is merged or abandoned.

Use a short, descriptive branch name, for example:

```text
feature/123-tcp-reconnect
fix/456-rdma-shutdown-race
perf/batched-rpc-send
docs/endpoint-lifetime
```

Commits should be logically organized and buildable. Write imperative commit
subjects that describe the change, and use the commit body to explain why when
the reason is not obvious. Do not mix unrelated cleanup, refactoring, behavior
changes, and performance optimization in one PR.

If you rewrite a branch that is already under review, notify reviewers. Use
`git push --force-with-lease`, never an unguarded force push.

## Pull Requests

Open a Draft PR early when design feedback or CI results would be useful. Mark
it ready for review only when its scope is stable and the relevant local checks
have passed.

A PR description must include:

- **Why:** the problem being solved and why the change is needed.
- **What changed:** the important implementation or behavior changes.
- **Scope:** what reviewers should focus on and what is intentionally excluded.
- **Validation:** exact tests, builds, platforms, and benchmarks that were run.
- **Compatibility:** public API, configuration, wire-format, or deployment
  implications, when applicable.
- **Limitations and follow-ups:** known remaining work, linked to issues.

Use `Closes #123` only when merging the PR into `main` completes that issue.
Use plain references such as `Refs #123` for partial or contextual work. GitHub
closing keywords operate when a PR is merged into the repository's default
branch.

Prefer small PRs with one primary review objective. In particular, separate:

- correctness changes from performance optimization;
- behavior changes from large-scale refactoring;
- generated or mechanical edits from hand-written logic;
- changes to independent subsystems with different reviewers or validation.

Keep the PR description current as the implementation changes. Resolve review
conversations only after the concern has been addressed or an explicit decision
has been recorded. Create an issue for required follow-up work before merging.

## Validation

Validation must match the affected code and risk. Report what you actually ran;
do not mark unavailable hardware tests as passed.

Run repository-wide formatting and static checks when possible:

```bash
python -m pip install pre-commit
pre-commit run --all-files
```

Build the Python wheel using the CPU-compatible configuration exercised by CI:

```bash
python -m pip install -U pip build wheel scikit-build-core pybind11 ninja
CMAKE_ARGS="-DBUILD_RDMA=ON -DBUILD_TCP=ON -DBUILD_PYTHON=ON \
  -DBUILD_NVLINK=OFF -DBUILD_TORCH_PLUGIN=OFF \
  -DBUILD_ASCEND_DIRECT=OFF -DBUILD_TEST=OFF -DUSE_CUDA=OFF" \
  python -m build --wheel --outdir dist dlslime
```

For Python changes, install the local package and run the relevant tests:

```bash
python -m pip install -e dlslime
python -m pytest dlslime/tests/python -v
```

Some tests require RDMA devices, GPUs, multiple processes, or platform-specific
software. Run the relevant hardware suite when you have access to it. Otherwise,
state the limitation in the PR so a maintainer or CI runner can provide the
missing validation.

For pure C++ changes, at minimum verify the affected configuration builds:

```bash
cmake -S dlslime -B build -GNinja -DBUILD_PYTHON=OFF -DBUILD_RDMA=ON
cmake --build build
```

Enable `BUILD_TEST=ON` and run the applicable C++ tests when modifying native
runtime behavior. Changes under `dlslime-ctrl/` must pass its Rust formatting,
check, and Clippy hooks through `pre-commit`.

Documentation changes should update all maintained copies of duplicated source
material when applicable and should build successfully using the instructions
in `docs/README.md`.

### Performance Changes

Performance claims require reproducible evidence. Include:

- hardware, topology, software versions, and relevant configuration;
- the exact benchmark command and workload;
- a baseline from an appropriate commit and the result from the proposed change;
- latency, bandwidth, throughput, memory, and CPU/GPU utilization as relevant;
- repeated measurements or an explanation of expected variance;
- confirmation that correctness tests still pass.

Use the benchmark entry points documented in `dlslime/bench/README.md`. Do not
trade correctness, resource safety, or portability for performance without an
explicitly reviewed design decision.

## Review and Merge

Authors are responsible for the complete change, including generated or
AI-assisted code. Review all submitted content for correctness, licensing,
security, and consistency with the project before opening a PR.

A PR is ready to merge when:

- its scope and behavior are understood and approved by an appropriate
  maintainer;
- required CI checks pass;
- all blocking review conversations are resolved;
- relevant documentation and tests are included;
- performance claims have reproducible evidence;
- compatibility risks and follow-up work are recorded.

DLSlime normally uses squash merge to keep `main` concise and easy to revert.
The squash commit title should describe the resulting change rather than the
review process. Direct pushes and force pushes to `main` should not be used.

After merging, verify that completed issues were closed, update any parent or
tracking issue, and delete the source branch.

## Issue and PR Maintenance

Maintainers periodically triage open work. Issues should have a clear type,
component, priority when needed, and enough information to reproduce or accept
the result. PRs without an owner, actionable next step, or recent activity may
be closed after a warning; useful work can be reopened when someone is ready to
continue it.

Be respectful, concise, and technical in discussions. Challenge ideas with
evidence, and assume good intent while keeping quality and user impact central.
