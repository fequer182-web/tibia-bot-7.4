// Exercise the scanner against actual MSVC containers in this executable.
// Including the implementation keeps calibration-only helpers out of the DLL API.
#include "../src/core/container_reader.cpp"

#include <array>
#include <deque>
#include <iostream>
#include <map>
#include <memory>
#include <stdexcept>

namespace otc {
bool copyLiveProfile(Profile&) { return false; }
void setContainerLayout(const ContainerLayout&) {}
} // namespace otc

namespace {

struct TestItem {
    virtual ~TestItem() = default;
    std::uint16_t id = 3031;
    std::uint16_t padding = 0;
    std::uint8_t count = 9;
};

struct TestContainer {
    virtual ~TestContainer() = default;
    int id = 3;
    int capacity = 8;
    std::string name = "Bag";
    std::deque<std::shared_ptr<TestItem>> items;
    std::array<std::uintptr_t, 4> invalidItems = {1, 1, 1, 1};
};

using ContainerMap = std::map<int, std::shared_ptr<TestContainer>>;
ContainerMap openContainers;
std::array<std::uintptr_t, 2> fakeHeader{};

void require(bool ok, const char* message) {
    if (!ok)
        throw std::runtime_error(message);
}

template <typename T>
std::uintptr_t address(const T* value) {
    return reinterpret_cast<std::uintptr_t>(value);
}

template <typename T, typename M>
std::int32_t memberOffset(const T& object, const M& member) {
    return static_cast<std::int32_t>(address(&member) - address(&object));
}

// Restore corrupted fixture bytes even if the expected rejection fails.
template <typename T>
class TemporaryValue {
public:
    TemporaryValue(std::uintptr_t addr, T value) : addr_(addr) {
        std::memcpy(&previous_, reinterpret_cast<const void*>(addr_), sizeof(T));
        std::memcpy(reinterpret_cast<void*>(addr_), &value, sizeof(T));
    }
    ~TemporaryValue() {
        std::memcpy(reinterpret_cast<void*>(addr_), &previous_, sizeof(T));
    }
    TemporaryValue(const TemporaryValue&) = delete;
    TemporaryValue& operator=(const TemporaryValue&) = delete;
private:
    std::uintptr_t addr_;
    T previous_{};
};

otc::ContainerLayout makeLayout(const TestContainer& container, const TestItem& item) {
    otc::ContainerLayout layout;
    layout.present = true;
    layout.collection = "map";
    layout.ptrSize = static_cast<int>(sizeof(void*));
    layout.itemPtrSize = static_cast<int>(sizeof(std::shared_ptr<TestItem>));
    layout.idOffset = memberOffset(container, container.id);
    layout.capacityOffset = memberOffset(container, container.capacity);
    layout.nameOffset = memberOffset(container, container.name);
    // This toolchain keeps a debug proxy as the first word even with the test's
    // release iterator setting. Point at the MSVC deque header used by the game.
    layout.itemsOffset = memberOffset(container, container.items) + static_cast<int>(sizeof(void*));
    layout.itemIdOffset = memberOffset(item, item.id);
    layout.itemCountOffset = memberOffset(item, item.count);
    otc::seedCollectionLayout(layout, otc::Profile{});
    return layout;
}

void checkSnapshot(std::uintptr_t base, const otc::ContainerLayout& layout, bool hasItems) {
    std::vector<otc::ContainerSnapshot> snapshots;
    require(otc::readCollection(base, layout, snapshots), "runtime collection read");
    require(snapshots.size() == 1, "null mapped slot is excluded from snapshots");
    const auto& snapshot = snapshots.front();
    require(snapshot.index == 3 && snapshot.name == "Bag" && snapshot.capacity == 8,
            "runtime snapshot keeps the real nonzero window index");
    require(snapshot.items.size() == (hasItems ? 1u : 0u), "runtime item count");
    if (hasItems) {
        require(snapshot.items.front().id == 3031 && snapshot.items.front().count == 9 &&
                    snapshot.items.front().slot == 0,
                "runtime item id, count and slot");
    }
}

void testModuleMapFallback() {
    const auto stale = std::make_shared<TestContainer>();
    const auto active = std::make_shared<TestContainer>();
    const auto item = std::make_shared<TestItem>();
    stale->items.push_back(item);
    active->items.push_back(item);
    openContainers.emplace(3, active);
    openContainers.emplace(7, nullptr);
    const auto base = address(&openContainers);
    const auto layout = makeLayout(*active, *item);
    const std::vector<otc::Needle> needles = {
        {address(stale.get()), layout}, {address(active.get()), layout}};
    otc::ContainerCalibHints hints;
    hints.name = "bAg";
    hints.capacity = 8;
    hints.hasItemId = true;
    hints.itemId = 3031;

    require(otc::inModule(base), "fixture map lives in the host module");
    require(otc::looksLikeContainerObject(address(active.get())), "polymorphic heap fixture");
    if (!otc::probeModuleContainerMap(base, layout, needles, hints)) {
        std::cerr << "fixture map=" << std::hex << base << " head="
                  << otc::nodePtr(base, 0) << std::dec << " sizeofMap=" << sizeof(ContainerMap)
                  << " layout id=" << layout.idOffset << " cap=" << layout.capacityOffset
                  << " name=" << layout.nameOffset << " items=" << layout.itemsOffset
                  << " itemId=" << layout.itemIdOffset << " count=" << layout.itemCountOffset
                  << " key=" << layout.mapNodeKeyOffset << " ptr=" << layout.mapNodePtrOffset
                  << " stride=" << layout.itemPtrSize << '\n';
        const auto dbgHead = otc::nodePtr(base, 0);
        const auto dbgRoot = otc::nodePtr(dbgHead, layout.mapNodeParentOffset);
        std::cerr << "sentinel nil=" << otc::readSized(dbgHead + layout.mapNodeIsnilOffset, 1)
                  << " root nil=" << otc::readSized(dbgRoot + layout.mapNodeIsnilOffset, 1)
                  << " root key=" << otc::readSized(dbgRoot + layout.mapNodeKeyOffset, 4)
                  << " root ptr=" << std::hex << otc::nodePtr(dbgRoot, layout.mapNodePtrOffset)
                  << " active=" << address(active.get()) << std::dec << '\n';
        otc::ContainerSnapshot dbgSnap;
        std::vector<std::uintptr_t> dbgItems;
        std::cerr << "readOne=" << otc::readOneContainer(address(active.get()), 3, layout, dbgSnap)
                  << " name=" << dbgSnap.name << " cap=" << dbgSnap.capacity
                  << " index=" << dbgSnap.index << " deque="
                  << otc::parseDequePtrs(address(&active->items) + sizeof(void*), layout.ptrSize,
                       layout.itemPtrSize, otc::dequeBlocks(layout), 0, 60, dbgItems)
                  << " items=" << dbgItems.size()
                  << " itemValid=" << otc::itemsLookValid(dbgItems, 0)
                  << " hint=" << otc::itemHintOk(dbgItems, layout, hints) << '\n';
    }
    require(otc::probeModuleContainerMap(base, layout, needles, hints),
            "active second needle is accepted with an additional null mapped node");
    require(!otc::probeModuleContainerMap(base, layout, {needles.front()}, hints),
            "same-name stale object alone cannot identify the open map");
    checkSnapshot(base, layout, true);

    otc::Profile profile;
    profile.inventory.present = true;
    profile.inventory.ptrSize = layout.itemPtrSize;
    profile.inventory.itemIdOffset = layout.itemIdOffset;
    profile.inventory.itemCountOffset = layout.itemCountOffset;
    otc::FoundCol found;
    require(otc::findModuleContainerMap(profile, needles, hints, found),
            "bounded module fallback finds the active needle beyond the stale first one");
    require(found.base == base && found.layout.collection == "map", "fallback selects real map");
    require(found.layout.chain.size() == 1 && !found.layout.chain.front().deref,
            "embedded module map saves a single non-dereferencing chain step");
    require(otc::resolveChain(found.layout.chain) == base, "saved chain resolves exactly");

    auto wrongHints = hints;
    wrongHints.itemId = 9999;
    require(!otc::probeModuleContainerMap(base, layout, needles, wrongHints),
            "wrong item hint is rejected");
    wrongHints = hints;
    wrongHints.capacity = 20;
    require(!otc::probeModuleContainerMap(base, layout, needles, wrongHints),
            "wrong capacity hint is rejected");
    wrongHints = hints;
    wrongHints.name = "Backpack";
    require(!otc::probeModuleContainerMap(base, layout, needles, wrongHints),
            "wrong name hint is rejected");

    active->id = 0;
    require(!otc::probeModuleContainerMap(base, layout, needles, hints),
            "map key must equal the nonzero Container id");
    active->id = -1;
    require(!otc::probeModuleContainerMap(base, layout, needles, hints),
            "invalid Container id cannot fall back to the map key during calibration");
    active->id = 3;
    auto paddingLayout = layout;
    paddingLayout.mapNodeKeyOffset = paddingLayout.mapNodePtrOffset - 4;
    if (paddingLayout.mapNodeKeyOffset != layout.mapNodeKeyOffset) {
        require(!otc::probeModuleContainerMap(base, paddingLayout, needles, hints),
                "x64 alignment padding cannot masquerade as window index zero");
    }

    const auto second = std::make_shared<TestContainer>();
    second->id = 7;
    second->name = "Backpack";
    second->capacity = 20;
    openContainers[7] = second;
    require(otc::probeModuleContainerMap(base, layout, needles, hints),
            "a second valid window may be empty and use another name/capacity");
    second->id = 0;
    require(!otc::probeModuleContainerMap(base, layout, needles, hints),
            "every non-null node must have matching window identity");
    openContainers[7].reset();

    std::uintptr_t head = 0;
    require(otc::readPtr(base, head), "fixture sentinel readable");
    const auto root = otc::nodePtr(head, layout.mapNodeParentOffset);
    {
        TemporaryValue<std::size_t> badCount(base + sizeof(void*), 1);
        require(!otc::probeModuleContainerMap(base, layout, needles, hints),
                "header count includes the null mapped node");
    }
    {
        TemporaryValue<std::uintptr_t> badParent(root + layout.mapNodeParentOffset, root);
        require(!otc::probeModuleContainerMap(base, layout, needles, hints),
                "broken root parent is rejected");
    }
    {
        TemporaryValue<std::uintptr_t> cyclicChild(root + layout.mapNodeLeftOffset, root);
        require(!otc::probeModuleContainerMap(base, layout, needles, hints),
                "cyclic child traversal is rejected within the bounded walk");
    }
    {
        TemporaryValue<std::uintptr_t> badMin(head + layout.mapNodeLeftOffset, head);
        require(!otc::probeModuleContainerMap(base, layout, needles, hints),
                "sentinel minimum must match the walked tree");
    }
    {
        TemporaryValue<std::uintptr_t> badMax(head + layout.mapNodeRightOffset, head);
        require(!otc::probeModuleContainerMap(base, layout, needles, hints),
                "sentinel maximum must match the walked tree");
    }
    fakeHeader = {head, 1};
    require(!otc::probeModuleContainerMap(address(&fakeHeader), layout, needles, hints),
            "readable fake module header with incorrect count is rejected");
    fakeHeader = {};
    const auto heapMap = std::make_unique<ContainerMap>(openContainers);
    require(!otc::probeModuleContainerMap(address(heapMap.get()), layout, needles, hints),
            "new module-only fallback rejects an otherwise valid heap collection");

    auto invalidLayout = layout;
    invalidLayout.itemsOffset = memberOffset(*active, active->invalidItems);
    const std::vector<otc::Needle> invalidNeedles = {{address(active.get()), invalidLayout}};
    require(!otc::probeModuleContainerMap(base, invalidLayout, invalidNeedles, hints),
            "invalid item parser cannot be accepted as an empty item list");

    active->items.clear();
    require(!otc::probeModuleContainerMap(base, layout, needles, hints),
            "calibration requires a nonempty matching fixture");
    checkSnapshot(base, layout, false);
    openContainers.clear();
    std::vector<otc::ContainerSnapshot> snapshots;
    require(otc::readCollection(base, found.layout, snapshots) && snapshots.empty(),
            "runtime continues to accept a calibrated map after all windows close");
    require(otc::resolveChain(found.layout.chain) == base, "closing windows preserves the anchor");
}

} // namespace

int main() {
    try {
        testModuleMapFallback();
        std::cout << "container calibration passed (pointer size " << sizeof(void*) << ")\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "container calibration failed: " << error.what() << '\n';
        return 1;
    }
}

