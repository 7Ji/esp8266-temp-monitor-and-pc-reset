#!/usr/bin/env python3
from shutil import copyfileobj, which
from subprocess import run
Import("env")

project_src_dir: str = env.subst("$PROJECT_SRC_DIR")
path_header: str = project_src_dir + "/root.html.h"
path_html: str = project_src_dir + "/root.html"

with open(path_header, "wb") as header:
    header.write(b'#pragma once\n\nstatic char const RootPage[] PROGMEM = R"HTML(')
    minify = which("minify")
    if minify is None:
        with open(path_html, "rb") as html:
            copyfileobj(html, header)
    else:
        header.write(run((minify, path_html), check=True, capture_output=True).stdout)
    header.write(b')HTML";\n')
