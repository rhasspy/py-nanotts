from setuptools import Extension, setup
import glob
import os

svoxpico_sources = glob.glob("nanotts/svoxpico/*.c")
lingware_files = sorted(glob.glob("nanotts/lang/*"))
data_files = []
if lingware_files:
    data_files.append((os.path.join("py_nanotts", "lang"), lingware_files))

ext_modules = [
    Extension(
        "py_nanotts._nanotts",
        sources=["py_nanotts/_nanotts.c"] + svoxpico_sources,
        include_dirs=["nanotts/svoxpico"],
        define_macros=[("Py_LIMITED_API", "0x03080000")],
        extra_compile_args=["-O2"],
        extra_link_args=["-lm"],
        py_limited_api=True,
    )
]

setup(
    name="py-nanotts",
    version="0.1.0",
    description="Python bindings for NanoTTS (PicoTTS)",
    packages=["py_nanotts"],
    ext_modules=ext_modules,
    data_files=data_files,
    zip_safe=False,
)
