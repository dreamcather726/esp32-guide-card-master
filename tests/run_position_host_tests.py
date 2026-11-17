"""Compile production positioning parser/subscriber using host cJSON and fake ESP I/O."""
import argparse
from pathlib import Path
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[1]


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--cjson", type=Path, required=True)
    args = parser.parse_args()
    with tempfile.TemporaryDirectory(prefix="jiuchuan-position-") as folder:
        temp = Path(folder)
        cjson = args.cjson.resolve()
        subprocess.run(["gcc", "-g", "-fsanitize=address,undefined", "-c", str(cjson / "cJSON.c"), "-o", str(temp / "cjson.o")], check=True)
        header = (ROOT / "main/boards/jiuchuan-s3/position_subscriber.h").read_text(encoding="utf-8")
        source = (ROOT / "main/boards/jiuchuan-s3/position_subscriber.cc").read_text(encoding="utf-8")
        cabinet = (ROOT / "main/boards/jiuchuan-s3/location_mqtt_client.cc").read_text(encoding="utf-8")
        manager_header = (ROOT / "main/boards/jiuchuan-s3/location_mqtt_client.h").read_text(encoding="utf-8")
        (temp / "manager.inc").write_text(manager_header[manager_header.index("class LocationMqttClient {"):manager_header.index("#endif")] + "\n" +
            cabinet[cabinet.index("void LocationMqttClient::Start()"):cabinet.index("bool LocationMqttClient::FetchCredentials(")], encoding="utf-8")
        (temp / "broker_log.inc").write_text(cabinet[cabinet.index("std::string BrokerAddressForLog("):
                                                    cabinet.index("constexpr const char* kCredentialsUrl")], encoding="utf-8")
        (temp / "subscriber.inc").write_text(header[header.index("class PositionSubscriber {"):] + "\n" + source[source.index("namespace {"):], encoding="utf-8")
        for name in ("position_data_test", "position_subscriber_test"):
            binary = temp / name
            subprocess.run(["g++", "-std=c++17", "-Wall", "-Wextra", "-Werror", "-g",
                            "-fsanitize=address,undefined", "-fno-omit-frame-pointer", "-no-pie",
                            "-I" + str(cjson), "-I" + folder, str(ROOT / "tests" / (name + ".cc")),
                            str(temp / "cjson.o"), "-o", str(binary)], check=True)
            subprocess.run([str(binary)], check=True)


if __name__ == "__main__":
    main()
