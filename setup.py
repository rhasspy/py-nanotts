from setuptools import Extension, setup
import glob
import os

ROOT = os.path.abspath(os.path.dirname(__file__))
svoxpico_sources = [
    os.path.join(ROOT, path)
    for path in glob.glob(os.path.join("nanotts", "svoxpico", "*.c"))
]
lingware_files = sorted(glob.glob(os.path.join("nanotts", "lang", "*")))

ext_modules = [
    Extension(
        "py_nanotts._nanotts",
        sources=["py_nanotts/_nanotts.c"] + svoxpico_sources,
        include_dirs=[os.path.join(ROOT, "nanotts", "svoxpico")],
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
    include_package_data=True,
    package_data={"py_nanotts": ["lang/*"]},
    zip_safe=False,
)
