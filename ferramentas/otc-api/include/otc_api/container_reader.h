#pragma once

#include "otc_api/types.h"

#include <cstdint>
#include <string>
#include <vector>

namespace otc {

struct ContainerCalibHints {
    std::string name;
    int capacity = 0;
    bool hasItemId = false;
    int itemId = 0;
};

void startContainerReader();

void setContainerCalibHints(const ContainerCalibHints& hints);
bool runContainerScan();
std::string containerCalibMessage();

std::string containersJson();
std::vector<ContainerSnapshot> currentContainers();
void replayContainersToPipe();

// Full MSVC shared_ptr (T* + control block) at an open-window slot. expectId 0 = any.
bool resolveContainerItem(int index, int slot, int expectId, std::uintptr_t& ptr,
                          std::uintptr_t& rep);

} // namespace otc

