from setuptools import Extension, setup
import glob
from pathlib import Path

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
    version="0.1.2",
    description="Python bindings for NanoTTS (PicoTTS)",
    long_description=Path("README.md").read_text(encoding="utf-8"),
    long_description_content_type="text/markdown",
    author="Michael Hansen",
    author_email="mike@rhasspy.org",
    url="https://github.com/rhasspy/py-nanotts",
    project_urls={
        "Source": "https://github.com/rhasspy/py-nanotts",
        "Changelog": "https://github.com/rhasspy/py-nanotts/blob/main/CHANGELOG.md",
    },
    license="Apache-2.0",
    classifiers=[
        "Programming Language :: Python :: 3",
        "Programming Language :: C",
        "Topic :: Multimedia :: Sound/Audio :: Speech",
    ],
    packages=["py_nanotts"],
    python_requires=">=3.9",
    ext_modules=ext_modules,
    include_package_data=True,
    package_data={"py_nanotts": ["lang/*"]},
    zip_safe=False,
)
