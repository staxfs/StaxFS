# Local (Single-Machine) Test Scripts

These scripts spin up the local 3 meta + 3 data topology described in
`conf/local/` and exercise it with various workload generators.

The actual server bring-up, teardown and per-workload runner logic is shared
with the distributed scripts under `scripts/distributed/`. Files here are thin
local wrappers that:

1. Build the project,
2. Stage `libdfs-hook.so`, `client.toml` and the workload binary into
   `/tmp/dfs-prototype/` (the absolute paths the distributed runners expect),
3. Launch the server cluster via `scripts/distributed/setup-test-servers.sh`,
4. Run a single local `mpirun` against the distributed runner script,
5. Tear everything down via `scripts/distributed/kill-test-servers.sh`.

Client `mpirun` is pinned to cores `64-127` so it does not
collide with the data servers (cores 0-29) or meta servers (32-37 / 42-47 / 52-57). All scripts must be invoked from the repo root.

## Usage

```bash
# bring up / tear down the cluster manually
sudo ./scripts/local/setup-test-servers.sh [LOG_LEVEL]    # default: info
./scripts/local/kill-test-servers.sh

# end-to-end workload runs (build + setup + workload + teardown)
sudo ./scripts/local/dev-run-mdtest.sh    ~/ior/src/mdtest          off
sudo ./scripts/local/dev-run-ior.sh       ~/ior/src/ior             off
sudo ./scripts/local/dev-run-filebench.sh ~/filebench/filebench \
    workloads/filebench/create_file.f off
sudo ./scripts/local/dev-run-cp.sh        ~/data/THUCNews    off
sudo ./scripts/local/dev-run-THUCTC.sh    ~/THUCTC ~/data/THUCNews off
```

Each `dev-run-*.sh` accepts an optional trailing `[ranks]` (and where
relevant `[repeats]` / `[parallel workloads]`) argument to override the
defaults.
