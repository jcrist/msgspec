import importlib
import json
import timeit
import resource
import sys
import gc

library, path = sys.argv[1:3]

with open(path, "rb") as f:
    json_data = f.read()

initial_rss = resource.getrusage(resource.RUSAGE_SELF).ru_maxrss

mod = importlib.import_module(f"benchmarks.bench_validation.bench_{library}")

msg = mod.decode(json_data)

gc.collect()
timer = timeit.Timer("func(data)", setup="", globals={"func": mod.encode, "data": msg})
n, t = timer.autorange()
encode_time = t / n

del msg

gc.collect()
timer = timeit.Timer(
    "func(data)", setup="", globals={"func": mod.decode, "data": json_data}
)
n, t = timer.autorange()
decode_time = t / n

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
