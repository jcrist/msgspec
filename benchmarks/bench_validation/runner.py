import gc
import importlib
import json
import resource
import sys
import timeit

library, path, runs, repeats = sys.argv[1:5]
num_runs = int(runs)
num_repeats = int(repeats)

if num_runs:

    def measure(timer):
        return min(timer.repeat(repeat=num_repeats, number=num_runs)) / num_runs
else:

    def measure(timer):
        n, t = timer.autorange()
        return t / n


with open(path, "rb") as f:
    json_data = f.read()

initial_rss = resource.getrusage(resource.RUSAGE_SELF).ru_maxrss

mod = importlib.import_module(f"benchmarks.bench_validation.bench_{library}")

msg = mod.decode(json_data)

gc.collect()
encode_time = measure(
    timeit.Timer("func(data)", setup="", globals={"func": mod.encode, "data": msg})
)

del msg

gc.collect()
decode_time = measure(
    timeit.Timer(
        "func(data)", setup="", globals={"func": mod.decode, "data": json_data}
    )
)

max_rss = resource.getrusage(resource.RUSAGE_SELF).ru_maxrss


report = json.dumps(
    {
        "label": mod.label,
        "encode": encode_time,
        "decode": decode_time,
        "memory": (max_rss - initial_rss) / 1024,
    }
)
print(report)
