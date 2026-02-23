from setuptools import Extension, setup
import glob

svoxpico_sources = glob.glob("nanotts/svoxpico/*.c")
lingware_files = sorted(glob.glob("nanotts/lang/*"))

ext_modules = [
    Extension(
        "py_nanotts._nanotts",
        sources=["py_nanotts/_nanotts.c"] + svoxpico_sources,
        include_dirs=["nanotts/svoxpico"],
        define_macros=[("Py_LIMITED_API", "0x03090000")],
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
    python_requires=">=3.9",
    ext_modules=ext_modules,
    include_package_data=True,
    package_data={"py_nanotts": ["lang/*"]},
    zip_safe=False,
)
