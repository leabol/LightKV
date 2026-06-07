import os
import re

# 1. Map all .hpp and .h files in src to their relative paths from src
src_dir = "src"
header_map = {}
for root, dirs, files in os.walk(src_dir):
    for f in files:
        if f.endswith(".hpp") or f.endswith(".h"):
            rel_path = os.path.relpath(os.path.join(root, f), src_dir)
            header_map[f] = rel_path

# 2. Go through all .hpp, .cpp in src, examples, tests
dirs_to_check = ["src", "examples", "tests"]
for d in dirs_to_check:
    for root, dirs, files in os.walk(d):
        for f in files:
            if f.endswith(".cpp") or f.endswith(".hpp") or f.endswith(".h"):
                filepath = os.path.join(root, f)
                with open(filepath, 'r') as fp:
                    content = fp.read()
                
                # We want to replace #include "..." if the file inside quotes matches a known header
                def replace_include(match):
                    inc_file = match.group(1)
                    # if the file is already a path like a/b.hpp, check if the basename is in map
                    basename = os.path.basename(inc_file)
                    if basename in header_map:
                        return f'#include "{header_map[basename]}"'
                    return match.group(0)
                
                new_content = re.sub(r'#include\s+"([^"]+)"', replace_include, content)
                
                if new_content != content:
                    print(f"Updated {filepath}")
                    with open(filepath, 'w') as fp:
                        fp.write(new_content)

