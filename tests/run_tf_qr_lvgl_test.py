"""Render the actual QR display methods with LVGL 9.3 (Linux/WSL, CMake, C++17)."""
import argparse
from pathlib import Path
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[1]


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--lvgl", type=Path, required=True, help="Extracted LVGL 9.3 source directory")
    args = parser.parse_args()
    source = (ROOT / "main/boards/jiuchuan-s3/jiuchuan_dev_board.cc").read_text(encoding="utf-8")
    display = source[source.index("    void SetChatMessage(const char* role"):
                     source.index("    void SetPreviewImage(std::unique_ptr<LvglImage> image)")]
    with tempfile.TemporaryDirectory(prefix="vanex-qr-lvgl-") as folder:
        temp = Path(folder)
        (temp / "tf_qr_display_production.inc").write_text(display, encoding="utf-8")
        (temp / "lv_conf.h").write_text(
            "#ifndef LV_CONF_H\n#define LV_CONF_H\n"
            "#define LV_COLOR_DEPTH 16\n#define LV_USE_STDLIB_MALLOC LV_STDLIB_CLIB\n"
            "#define LV_USE_OS LV_OS_NONE\n#define LV_DRAW_SW_COMPLEX 1\n"
            "#define LV_CACHE_DEF_SIZE 0\n#define LV_IMAGE_HEADER_CACHE_DEF_CNT 0\n#endif\n",
            encoding="utf-8")
        (temp / "CMakeLists.txt").write_text(
            'cmake_minimum_required(VERSION 3.16)\nproject(qr_lvgl_test C CXX)\n'
            'set(CMAKE_CXX_STANDARD 17)\n'
            'set(CONFIG_LV_BUILD_DEMOS OFF CACHE BOOL "" FORCE)\n'
            'set(CONFIG_LV_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)\n'
            'set(CONFIG_LV_USE_THORVG_INTERNAL OFF CACHE BOOL "" FORCE)\n'
            f'add_subdirectory("{args.lvgl.resolve().as_posix()}" lvgl)\n'
            f'add_executable(qr_lvgl_test "{(ROOT / "tests/tf_qr_lvgl_test.cc").as_posix()}")\n'
            'target_include_directories(qr_lvgl_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})\n'
            'target_link_libraries(qr_lvgl_test PRIVATE lvgl m)\n', encoding="utf-8")
        for command in (
            ["cmake", "-S", folder, "-B", str(temp / "build")],
            ["cmake", "--build", str(temp / "build"), "--parallel", "4"],
        ):
            result = subprocess.run(command, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True)
            if result.returncode:
                print(result.stdout[-12000:])
                result.check_returncode()
        subprocess.run([str(temp / "build/qr_lvgl_test")], cwd=temp, check=True)


if __name__ == "__main__":
    main()
