#pragma once
#include <string>

struct NavigationStatus {
    bool has_fix = false;
    bool has_coordinates = false;
    double latitude = 0;
    double longitude = 0;
    int satellites = -1;
    std::string scene;
};
