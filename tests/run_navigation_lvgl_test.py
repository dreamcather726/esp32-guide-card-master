"""Render the production four-corner overlay with real LVGL 9.3; no firmware build."""
from pathlib import Path
import argparse
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[1]
parser = argparse.ArgumentParser()
parser.add_argument("--lvgl", type=Path, required=True)
parser.add_argument("--cjson", type=Path, required=True)
parser.add_argument("--image", type=Path)
args = parser.parse_args()
with tempfile.TemporaryDirectory(prefix="jiuchuan-navigation-") as folder:
    temp = Path(folder)
    (temp / "lv_conf.h").write_text(
        "#ifndef LV_CONF_H\n#define LV_CONF_H\n#define LV_COLOR_DEPTH 16\n"
        "#define LV_USE_STDLIB_MALLOC LV_STDLIB_CLIB\n#define LV_USE_OS LV_OS_NONE\n"
        "#define LV_FONT_MONTSERRAT_32 1\n#define LV_DRAW_SW_COMPLEX 1\n"
        "#define LV_CACHE_DEF_SIZE 0\n#define LV_IMAGE_HEADER_CACHE_DEF_CNT 0\n#endif\n")
    (temp / "CMakeLists.txt").write_text(
        'cmake_minimum_required(VERSION 3.16)\nproject(navigation_test C CXX)\n'
        'set(CMAKE_CXX_STANDARD 17)\n'
        'set(CONFIG_LV_BUILD_DEMOS OFF CACHE BOOL "" FORCE)\n'
        'set(CONFIG_LV_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)\n'
        'set(CONFIG_LV_USE_THORVG_INTERNAL OFF CACHE BOOL "" FORCE)\n'
        f'add_subdirectory("{args.lvgl.resolve().as_posix()}" lvgl)\n'
        f'add_executable(navigation_test "{(ROOT / "tests/navigation_overlay_test.cc").as_posix()}")\n'
        f'target_include_directories(navigation_test PRIVATE "{args.cjson.resolve().as_posix()}")\n'
        'target_link_libraries(navigation_test PRIVATE lvgl m)\n')
    for command in (["cmake", "-S", folder, "-B", str(temp / "build")],
                    ["cmake", "--build", str(temp / "build"), "--parallel", "4"]):
        result = subprocess.run(command, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True)
        if result.returncode:
            print(result.stdout[-10000:]); result.check_returncode()
    command = [str(temp / "build/navigation_test")]
    if args.image:
        args.image.resolve().parent.mkdir(parents=True, exist_ok=True)
        command.append(str(args.image.resolve()))
    subprocess.run(command, check=True)
