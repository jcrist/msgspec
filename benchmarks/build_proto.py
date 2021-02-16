import glob
import os
import shutil

# Hacky, I know
os.system("pyrobuf --proto3 --package proto_bench bench.proto")
sos = glob.glob("build/*/*.so")
assert len(sos) == 1
shutil.move(sos[0], ".")
