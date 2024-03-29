from distutils.core import setup
from Cython.Build import cythonize
from distutils.extension import Extension
import numpy as np
import os

MODULE_NAME = "bm25"

COMPILER_FLAGS = [
    "-std=c++17",
    "-stdlib=libc++",
    "-O3",
    "-Wall",
    "-Wextra",
    "-march=native",
    "-ffast-math",
    ## "-fPIC",
]

OS = os.uname().sysname

LINUX_LINK_ARGS = [
        ## "-fopenmp",
    "-lc++",
    "-lc++abi",
    ## "-lleveldb",
    "-L/usr/local/lib",
    ## "-lsnappy",
]

DARWIN_LINK_ARGS = [
    "-lc++",
    "-lc++abi",
    ## "-lleveldb",
    "-L/usr/local/lib",
    ## "-lsnappy",
]

extensions = [
    Extension(
        MODULE_NAME,
        sources=["bm25/bm25.pyx", "bm25/engine.cpp"],
        extra_compile_args=COMPILER_FLAGS,
        language="c++",
        include_dirs=[np.get_include(), "bm25"],
        extra_link_args=DARWIN_LINK_ARGS if OS == "Darwin" else LINUX_LINK_ARGS,
    ),
]

setup(
    name=MODULE_NAME,
    ext_modules=cythonize(extensions),
)
