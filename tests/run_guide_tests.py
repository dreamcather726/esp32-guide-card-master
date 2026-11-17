"""Run production guide state and real LVGL/CJK page rendering on Linux/WSL."""
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
with tempfile.TemporaryDirectory(prefix="jiuchuan-guide-") as folder:
    temp = Path(folder)
    (temp / "lv_conf.h").write_text(
        "#ifndef LV_CONF_H\n#define LV_CONF_H\n#define LV_COLOR_DEPTH 16\n"
        "#define LV_USE_STDLIB_MALLOC LV_STDLIB_CLIB\n#define LV_USE_OS LV_OS_NONE\n"
        "#define LV_FONT_MONTSERRAT_48 1\n#define LV_DRAW_SW_COMPLEX 1\n"
        "#define LV_FONT_FMT_TXT_LARGE 1\n"
        "#define LV_CACHE_DEF_SIZE 0\n#define LV_IMAGE_HEADER_CACHE_DEF_CNT 0\n#endif\n")
    (temp / "CMakeLists.txt").write_text(
        'cmake_minimum_required(VERSION 3.16)\nproject(guide_test C CXX)\n'
        'set(CMAKE_CXX_STANDARD 17)\n'
        'set(CONFIG_LV_BUILD_DEMOS OFF CACHE BOOL "" FORCE)\n'
        'set(CONFIG_LV_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)\n'
        'set(CONFIG_LV_USE_THORVG_INTERNAL OFF CACHE BOOL "" FORCE)\n'
        f'add_subdirectory("{args.lvgl.resolve().as_posix()}" lvgl)\n'
        f'add_executable(guide_pages_test "{(ROOT / "tests/guide_pages_test.cc").as_posix()}" '
        f'"{(ROOT / "managed_components/78__xiaozhi-fonts/src/font_puhui_basic_20_4.c").as_posix()}" '
        f'"{(ROOT / "managed_components/78__xiaozhi-fonts/src/font_puhui_20_4.c").as_posix()}")\n'
        f'target_include_directories(guide_pages_test PRIVATE "{args.cjson.resolve().as_posix()}")\n'
        'target_link_libraries(guide_pages_test PRIVATE lvgl m)\n'
        f'add_executable(guide_state_test "{(ROOT / "tests/guide_state_test.cc").as_posix()}")\n')
    for command in (["cmake", "-S", folder, "-B", str(temp / "build")],
                    ["cmake", "--build", str(temp / "build"), "--parallel", "8"]):
        result = subprocess.run(command, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True)
        if result.returncode:
            print(result.stdout[-10000:]); result.check_returncode()
    subprocess.run([str(temp / "build/guide_state_test")], check=True)
    command = [str(temp / "build/guide_pages_test")]
    if args.image:
        args.image.resolve().parent.mkdir(parents=True, exist_ok=True)
        command.append(str(args.image.resolve()))
    subprocess.run(command, check=True)
