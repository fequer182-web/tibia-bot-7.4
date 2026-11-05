#pragma once

#include "otc_api/types.h"

#include <string>
#include <vector>

namespace otc {

struct MapCalibHints {
    bool hasItemId = false;
    int itemId = 0;
};

void startMapReader();

void setMapCalibHints(const MapCalibHints& hints);
bool runMapScan();
std::string mapCalibMessage();

std::string mapJson();
std::vector<TileSnapshot> currentMapTiles();

} // namespace otc

