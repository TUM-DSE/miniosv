# Running the packaged apps

One application is linked into the kernel, so *which app* is a build-time choice and *which
program inside it* is a boot-time one:

```bash
make app=app/miniduckdb              # -> build/release.x64/loader.img
make app=app/llama.cpp arch=aarch64  # -> build/release.aarch64/loader.img
```

`scripts/run.py --args "<executable> [args...]"` writes those words into the boot image before
starting QEMU; the first word names the executable, the rest is its `argv`. Run with no `--args`
to print the list.

## Data disks

`--emulated-nvme FILE` attaches a file as NVMe controller 1, 2, … in the order given (0 is the
boot disk). The file is either an ext4 image, which the app mounts (`/db` for DuckDB, `/root` for
llama.cpp), or a plain file the app reads as a raw namespace, named `nvme:<id>`.

```bash
scripts/mkdata.sh build/data.img [staging-dir] [size]     # ext4 image, no root needed
```

After an unclean shutdown the image needs `fsck.ext4 -f -y build/data.img` — there is no journal.

## DuckDB: TPC-H

Executables: `duckdb` (upstream CLI), `benchmark` (upstream runner), `sql` (minimal `-d`/`-c`).

### Pre-generated parquet, any scale factor

`miniosv/mk-tpch.sh` builds a data image from directories of TPC-H parquet files — one
`tpch<SF>/` per scale factor, each with the eight tables as `<table>.parquet` — and generates a
benchmark group that queries them. Files are hardlinked into the staging tree when it lands on the
same filesystem, so nothing is copied twice.

```bash
make app=app/miniduckdb
app/miniduckdb/miniosv/mk-tpch.sh --src [tpch_data_folder] --img [data_img] 1 10

scripts/run.py --image-path build/duckdb.x64/loader.img -m 32G --emulated-nvme [data_img] \
    --args "benchmark --sf 10 benchmark/tpch-parquet/q06.benchmark"        # one query
scripts/run.py --image-path build/duckdb.x64/loader.img -m 32G --emulated-nvme [data_img] \
    --args "benchmark --sf 10 'benchmark/tpch-parquet/q[0-9]+.benchmark'"  # all 22
```

The scale factor is a run-time argument, so one image carrying several of them serves all. Each
query runs five times and prints a timing per run; `--threads=n` and `--memory_limit=n` bound the
engine, `--list` shows every benchmark found. A benchmark is named by its path, not by the `name`
line inside it, and the pattern is a full-match regex.

Answers ship for sf1 and sf100 only, so those runs verify their results and the rest are timed but
unchecked.

### Generating in the guest instead

```bash
scripts/mkdata.sh build/data.img
scripts/run.py -m 6G --emulated-nvme build/data.img \
    --args "duckdb /db/tpch.duckdb -c 'CALL dbgen(sf=0.1)' -c 'PRAGMA tpch(1)'"
```

`PRAGMA tpch(N)` runs query N (1–22), `FROM tpch_queries()` lists them. The database stays on the
image, so a later boot skips `dbgen`. Without `-c` the CLI gives an interactive prompt; `.quit`
exits.

### Limits worth knowing

- Boot arguments are capped at 447 bytes ([setargs.py:26](scripts/setargs.py#L26)).
- The CLI's `-f` and `-init` go through libc `fopen`, which always fails here. SQL reaches the CLI
  through `-c` only; anything longer belongs in a benchmark file, which is read through miniext.
- `glob()` is unsupported, so `read_parquet('/db/tpch1/*.parquet')` does not work — name each file.

## llama.cpp

Executables: `llama-cli`, `batched-bench`, `lros-serve`. All `exit()` when done, which powers the
VM off.

miniOSv has no load balancer, so a thread stays on the core it starts on. ggml's own threads must
be pinned with `-C <mask> --cpu-strict 1`, one bit per vcpu (`f` for the default `-c 4`, `ff` for
`-c 8`), or they all run on one core. `--lros` and `lros-serve --mode lros` pin their own workers.

```bash
make app=app/llama.cpp
MODEL=$(app/llama.cpp/miniosv/get-model.sh llama-3.2-1b-q8)   # --list for the others

scripts/run.py -m 6G --emulated-nvme "$MODEL" \
    --args "llama-cli -m nvme:1 -no-cnv -p 'The capital of France is' -n 32 -t 4 -C f --cpu-strict 1"

scripts/run.py -m 6G --emulated-nvme "$MODEL" \
    --args "batched-bench -m nvme:1 -c 2048 -npp 16,32 -ntg 128,256 -npl 1,4 -t 4 -C f --cpu-strict 1"
```

The model can equally live on an ext4 image (`mkdata.sh` with a staging dir holding `models/`),
then `-m /root/models/<name>.gguf`.

### Options before the executable

Taken by the launcher ([main.cc](app/llama.cpp/miniosv/main.cc)), not by llama.cpp, and applying to
whichever executable follows.

| option | effect |
|---|---|
| `--cache-size <n>[K\|M\|G]` | memory the model's page cache may hold. Never 0: it then never evicts, and a fault that cannot allocate raises SIGSEGV |
| `--buffer-size <n>[K\|M\|G]` | how much one fault brings in |
| `--prefetch-depth <n>` | buffers read ahead of each fault, 0 to disable |
| `--per-tensor-buffers` | cut buffers at tensor boundaries instead of at `--buffer-size` |
| `--buffer-cap <n>` | with the above, split a tensor bigger than 1/n of the cache; 0 to not |
| `--pin-percent <n>` | hold that much of the cache resident as a prefix, stream the rest |
| `--env NAME=VALUE` | set an environment variable, repeatable |
| `--no-poweroff` | stay up after the app returns, for a console |

`--env` is how the ggml backends get their config: they read `getenv()`, the guest has no shell,
and with a raw model on the data disk there is no file to read either. The usage text printed by an
empty `--args` lists all of these except `--env`.

### lros-serve

A serving loop driven by a request trace, derived from `examples/parallel`. Requests arrive at the
trace's times with a priority and a token limit, batch into one `llama_batch` per iteration, and a
higher-priority arrival preempts a lower-priority request at the next iteration boundary. Host
binary `llama-lros-serve`, guest executable `lros-serve`.

Its own options come off `argv` first, so everything upstream accepts (`-m`, `-c`, `-np`, `-t`,
sampling) still works.

| option | meaning |
|---|---|
| `--trace <file>` | required; format below |
| `--mode slots\|lros` | `slots` (default) is userspace slots as inference servers do it, preemption by parking the victim's KV in a spare sequence. `lros` is the inference-task path, where the OS queues, places and runs the iterations; needs the miniOSv build (`LLAMA_LROS`) |
| `--policy <file>` | JSON policy, next section; absent means built-in defaults |
| `--models a,b,c` | extra models beyond `-m`, which is index 0. Weights only, each task builds its own context |

`-np` sets the slot count in `slots` mode, not the sequence count: the loop adds parking sequences
and one for the system prompt, and `lros` mode instead takes one sequence per trace request,
because there a request owns its sequence for its whole life.

```bash
scripts/run.py -m 6G --emulated-nvme build/data.img --emulated-nvme "$MODEL" \
    --args "lros-serve -m nvme:2 --trace /root/traces/priority-mix.trace \
            --policy /root/policies/rk3588-npu-llama32-1b-f16.json --mode lros -np 4 -t 4"
```

Trace and policy are read through miniext, so they need an ext4 data disk; `mkdata.sh` with a
staging dir holding `traces/` and `policies/` puts them there. Examples of both ship in
[examples/lros-serve/](app/llama.cpp/examples/lros-serve/).

#### Trace format

One request per line, `#` comments, sorted by arrival, ids in file order:

```
<t_arrival_ms> <model> <priority> <max_tokens> [prompt text to end of line]
```

`model` indexes `--models`, 0 with one model. `priority` is lower-is-higher. No prompt takes one
from the built-in list. (`--help` still prints the older four-column form without `model`.)

#### The JSON policy file

Where the per-model, per-machine calibration lives, so changing board or model does not mean
changing code. `batched-bench --lros` produces the width numbers.

Every section and field is optional; anything absent keeps the default. An unreadable file warns,
invalid JSON logs the parser error, and both fall back to the defaults — neutral ones: every phase
asks for every core, no accelerator, batching by llama.cpp's capacity rule alone. Unknown keys are
ignored, which is what makes the `comment`/`_comment` arrays in the shipped policies work; they
carry the measurements behind each number and are worth keeping.

```json
{
  "comment": ["free text, ignored"],
  "width":   { "prefill": [ { "upto_batch": 1048576, "workers": 0 } ],
               "decode":  [ { "upto_batch": 1, "workers": 2 },
                            { "upto_batch": 3, "workers": 3 },
                            { "upto_batch": 1048576, "workers": 0 } ] },
  "accel":   { "capacity": 3,
               "prefill": [ { "upto_batch": 1048576, "workers": 3 } ],
               "decode":  [ { "upto_batch": 1048576, "workers": 3 } ] },
  "floor":   { "cores": 1, "long_work_tokens": 64, "max_pause_ms": 2000 },
  "context": { "n_seq_per_task": 8, "n_ctx_per_seq": 512 },
  "batching":{ "same_priority_only": false, "max_batch": 0 },
  "memory":  { "budget_mb": 1400, "kv_evict": "auto",
               "kv_swap_mib_per_s": 0, "prefill_tokens_per_s": 0 }
}
```

| field | default | meaning |
|---|---|---|
| `width.prefill`, `width.decode` | every core | CPU workers per phase. Rules sort by `upto_batch` and the first one at least as large as the batch wins; `workers: 0` is "as many as the scheduler gives". The decode shape above is the point: on four A76 cores decode at batch 1 saturates at 2 workers (6.4/9.0/8.6 t/s at 1/2/4) |
| `accel.capacity` | `0` (domain off) | units the device has — 3 for the RK3588 NPU, whose cores are separately addressable and each do real work (2.81x on three); 1 for a GPU, which has no partition and where more units would mean time-sharing |
| `accel.prefill`, `accel.decode` | none | same rule form. Separate from `width` rather than a factor on it, because the domains disagree on what a unit buys. Empty means no accelerator, which is what a CPU-only image and an uncalibrated platform want |
| `floor.cores` | `1` | cores left for lower-priority work; 0 restores strict priority |
| `floor.long_work_tokens` | `64` | prompt+remaining above this counts as long work and leaves the floor |
| `floor.max_pause_ms` | `2000` | leave the floor regardless once something has waited this long |
| `context.n_seq_per_task` | `8` | sequences a task's context holds |
| `context.n_ctx_per_seq` | `512` | tokens **per sequence**, not per context: llama.cpp splits `n_ctx` evenly, and the total is `n_ctx_per_seq * (n_seq_per_task + 1)`, the extra sequence holding the system prompt. Too small fails silently — 227 tokens against a 259-token system prompt collapsed throughput 17x |
| `batching.same_priority_only` | `false` | keep a latency-sensitive request out of a batch of background work, at the cost of two tasks over one model. On `priority-mix`/RK3588: `false` 22.6 t/s aggregate, prio-0 at 5.9; `true` 8.5 aggregate, prio-0 at 8.8 |
| `batching.max_batch` | `0` | hard cap on members, 0 for whatever the context allows |
| `memory.budget_mb` | `0` (unmanaged) | machine memory in MiB. The weight cache's allowance is charged against it, so vary this figure rather than a difference of two |
| `memory.kv_evict` | `swap` | what happens to a task's KV when the budget takes its context. `swap` serialises every live sequence and puts it back; `recompute` keeps nothing and the request re-processes its prompt and generation, as a slot engine does; `auto` picks the cheaper per request. Any other value warns and uses `swap` |
| `memory.kv_swap_mib_per_s`, `memory.prefill_tokens_per_s` | `0` | calibration for `auto`; 0 uses what the run has measured so far |

### batched-bench

Upstream, plus `--lros`: compute every graph on workers lros creates and pins, the same path
`lros-serve --mode lros` uses, so a sweep here calibrates a policy's `width` rules rather than
measuring a different mechanism. miniOSv build only.

```bash
scripts/run.py -m 6G --emulated-nvme "$MODEL" \
    --args "batched-bench -m nvme:1 --lros -npp 128 -ntg 32 -npl 1,4 -t 4"
```

### vAccel

Matmul offload to a host accelerator, built in by default (`conf_vaccel=0` for a CPU-only image):

```bash
make app=app/llama.cpp arch=aarch64 conf_vaccel=1
scripts/run.py --arch aarch64 --vaccel -m 6G \
    --emulated-nvme build/data.img --emulated-nvme "$MODEL" \
    --args "llama-cli -m nvme:2 -no-cnv -p 'What is a combustion engine' -n 32 -t 4 -C f --cpu-strict 1"
```

Needs the QEMU from the `lros-qemu` flake (`nix develop` sets `$QEMU_VACCEL`) and
`config/mat_kernel_size.json` on the data disk — the backend reads `/root/config` to decide which
matrix shapes it may offload. `--env` overrides individual fields of that file, and wins over it:
`NPU_PREFILL`, `NPU_DECODE` (0/1/true/false), `OFFLOAD_NODES`, `LOADED_NODES` (JSON arrays of node
names), `NUM_CORES`, `OMP_THREADS` (integers).

### VIAI

The graph-level successor to vAccel: whole ggml subgraphs, not one matmul at a time. Build with
`conf_viai=1` (and `conf_vaccel=0`); the host side is the plugin QEMU dlopens.

| `--env` | meaning |
|---|---|
| `VIAI_POLICY` | what the guest *may* offload, JSON, below |
| `VIAI_STATS` | report the largest serialised graph against the one-request cap, so the headroom is measurable per model |

`VIAI_POLICY` is a boot option rather than a file because the data disk is a raw model, so there is
nothing to read. It answers a different question from the device's own `supports_op`: that says
what the device *can* run, this says what it is *allowed* to run.

```
--env VIAI_POLICY={ops:[MUL_MAT],nodes:ffn.*,min_batch:2}
```

| field | effect when absent |
|---|---|
| `ops` | array of ggml op names (`MUL_MAT`, …); any op qualifies |
| `nodes` | regex matched against the node name; any node qualifies |
| `min_batch` | keep `MUL_MAT` with `ne[1]` below this on the CPU, since a device that wins at batch 128 can lose at batch 1; no lower bound |

Quotes survive neither the shell, ssh nor the boot-argument chain, so the parser accepts the
quote-free form above and re-quotes bare tokens; the quoted form works when it does survive. A
value containing `:` or `,` cannot be written this way. A policy that fails to parse aborts rather
than defaulting to "offload everything", which would look like a working run.
